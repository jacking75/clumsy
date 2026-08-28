// Linux capture backend - libnetfilter_queue  (Phase 4.2)
//
// Mirrors divert.cpp's structure exactly (read loop + clock loop + one mutex),
// so the module contract and the timing behaviour are identical to Windows.
//
// How a packet flows:
//   iptables ... -j NFQUEUE --queue-num N   (the operator installs this)
//     -> nfq callback -> filterMatch() -> createNode -> appendNode
//     -> divertConsumeStep -> modules -> sendAllListPackets
//     -> nfq_set_verdict(NF_ACCEPT) for survivors, NF_DROP for the rest
//
// Two things differ from WinDivert and drive the design here:
//
//  1. Every queued packet id MUST receive exactly one verdict. A packet a module
//     drops still has to be answered with NF_DROP or the kernel queue stalls, so
//     freeNode() calls packetBackendOnFree() and we settle it there.
//
//  2. A queued id can only be verdicted once, so duplicate.cpp's clones cannot
//     ride on the original's id. Clones are marked synthetic and re-injected
//     through a raw socket instead.
//
//     That re-injection re-enters the OUTPUT chain, so without help it hits the
//     very NFQUEUE rule that produced it and every clone gets cloned again -
//     measured at 10 packets in, 246106 out before this was addressed. Two
//     defences: the injection socket carries an fwmark the operator's rules can
//     ACCEPT ahead of the NFQUEUE rule, and an injection rate cap turns a
//     missing bypass rule into a bounded, self-diagnosing failure instead of a
//     packet storm.
//
// Include order matters: the glibc network headers must precede
// <linux/netfilter.h>, otherwise struct in_addr gets defined twice.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"
#include "filterexpr.h"

// iptables_linux.cpp
int  iptablesAutoInstall(const FilterProgram *prog, int queueNum, UINT32 injectMark);
void iptablesAutoRemove(void);
int  iptablesAutoIsEnabled(void);

#define MAX_PACKETSIZE 0xFFFF
// same clock cadence as the Windows backend so module timing matches
#define CLOCK_WAITMS 40
// Must exceed the deepest module buffer (lag/bandwidth keep up to 2000) or the
// kernel starts dropping packets behind our back while we hold them.
#define NFQ_QUEUE_MAXLEN 8192
#define NFQ_RECV_BUFSIZE (4 * 1024 * 1024)
// Arbitrary but distinctive; --inject-mark overrides it if it clashes with an
// existing firewall policy.
#define DEFAULT_INJECT_MARK 0xC1

// ---------------------------------------------------------------------------
// Backend-private per packet state, stored in PacketNode::backend
// ---------------------------------------------------------------------------
typedef struct {
    UINT32 packetId;       // NFQUEUE id; 0 means "synthetic, not from the queue"
    UINT8  verdictIssued;  // 1 once accept/drop has been sent for packetId
    UINT8  synthetic;      // 1 = injected by us, send via the raw socket
} LinuxPacketMeta;

static_assert(sizeof(LinuxPacketMeta) <= PACKET_BACKEND_META_SIZE,
              "PACKET_BACKEND_META_SIZE is too small for LinuxPacketMeta");

static INLINE_FUNCTION LinuxPacketMeta* nodeMeta(PacketNode *node) {
    return (LinuxPacketMeta*)node->backend.raw;
}

// ---------------------------------------------------------------------------
// Module-visible globals, same names as the Windows backend
// ---------------------------------------------------------------------------
volatile LONG statsCapturedTotal = 0;
volatile LONG statsSentTotal = 0;

static struct nfq_handle   *nfqHandle  = NULL;
static struct nfq_q_handle *nfqQueue   = NULL;
static int                  nfqFd      = -1;
static int                  rawSocket   = -1;  // AF_INET,  IP_HDRINCL
static int                  rawSocket6  = -1;  // AF_INET6, kernel writes the header
static long                 rawDropCount = 0;   // clones dropped, raw socket full
static UINT32               injectMark = 0;     // fwmark stamped on injected packets
// Runaway guard: injections per second, and the window they are counted in.
#define INJECT_RATE_LIMIT 5000
static long                 injectWindowCount = 0;
static DWORD                injectWindowStart = 0;
static short                injectDisabled = 0;
static int                  queueNum   = 0;
static volatile short       stopLooping;
static HANDLE               loopThread, clockThread, mutex;
static FilterProgram       *filterProg = NULL;

extern PacketNode * const head;
extern PacketNode * const tail;

static DWORD divertReadLoop(LPVOID arg);
static DWORD divertClockLoop(LPVOID arg);

// ---------------------------------------------------------------------------
// Verdicts
// ---------------------------------------------------------------------------

// Answers the kernel for one queued packet. Safe to call more than once; only
// the first call reaches the kernel.
static void settleVerdict(PacketNode *node, int verdict) {
    LinuxPacketMeta *m = nodeMeta(node);
    if (m->synthetic || m->verdictIssued || m->packetId == 0 || !nfqQueue) return;
    m->verdictIssued = 1;

    if (verdict == NF_ACCEPT) {
        // Hand the (possibly modified) bytes back so tamper/reset take effect.
        nfq_set_verdict(nfqQueue, m->packetId, NF_ACCEPT,
                        node->packetLen, (unsigned char*)node->packet);
    } else {
        nfq_set_verdict(nfqQueue, m->packetId, NF_DROP, 0, NULL);
    }
}

void packetBackendOnFree(PacketNode *node) {
    // Reached for packets a module discarded and for packets already sent.
    // settleVerdict() is idempotent, so the already-sent case is a no-op and
    // the discarded case becomes the NF_DROP the kernel is waiting for.
    settleVerdict(node, NF_DROP);
}

void packetBackendPrepareClone(const PacketNode *src, PacketNode *dst) {
    LinuxPacketMeta *m = nodeMeta(dst);
    UNREFERENCED_PARAMETER(src);
    memset(m, 0, sizeof(*m));
    m->synthetic = 1;   // no queue id of its own; goes out on the raw socket
}

// --- replay injection (T7) ---
//
// pcap replay needs to send packets when no capture is running, so it cannot
// borrow the capture sockets: those only exist between divertStart() and
// divertStop(). It gets its own pair with the same fwmark, opened lazily on
// the replay thread. pcapReplayStop() joins that thread before calling the
// close below, so this needs no lock.
static int replaySocket  = -1;
static int replaySocket6 = -1;

static int ensureReplaySockets(void) {
    const int one = 1;
    const int sndbuf = 4 * 1024 * 1024;
    UINT32 mark;

    if (replaySocket >= 0 || replaySocket6 >= 0) return 1;

    mark = (UINT32)argGetInt("inject-mark", DEFAULT_INJECT_MARK);

    replaySocket = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_RAW);
    if (replaySocket >= 0) {
        setsockopt(replaySocket, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));
        setsockopt(replaySocket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        // Same reasoning as the capture injection socket: without the mark, a
        // replayed packet walks straight back into the operator's NFQUEUE rule.
        setsockopt(replaySocket, SOL_SOCKET, SO_MARK, &mark, sizeof(mark));
    }

    replaySocket6 = socket(AF_INET6, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_RAW);
    if (replaySocket6 >= 0) {
        setsockopt(replaySocket6, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(replaySocket6, SOL_SOCKET, SO_MARK, &mark, sizeof(mark));
    }

    if (replaySocket < 0 && replaySocket6 < 0) {
        INFO("replay: cannot open a raw socket (%s). CAP_NET_RAW is required.",
             strerror(errno));
        return 0;
    }
    return 1;
}

int packetBackendInject(char *packet, UINT len, BOOL outbound) {
    ssize_t sent;

    if (!packet || len < sizeof(struct iphdr)) return 0;

    if (!outbound) {
        // Same limit duplicate.cpp lives with: a raw socket can only originate
        // traffic. Delivering into the local receive path would need a TUN
        // device or an ifb redirect, which is out of scope.
        LOG("replay: inbound injection is not supported on Linux");
        return 0;
    }
    if (!ensureReplaySockets()) return 0;

    if ((((unsigned char)packet[0]) >> 4) == 6) {
        const struct ip6_hdr *ip6 = (const struct ip6_hdr*)packet;
        struct sockaddr_in6 dst6;

        if (replaySocket6 < 0 || len <= sizeof(struct ip6_hdr)) return 0;
        memset(&dst6, 0, sizeof(dst6));
        dst6.sin6_family = AF_INET6;
        dst6.sin6_addr   = ip6->ip6_dst;
        // No IP_HDRINCL for IPv6: hand the kernel the payload and let it
        // rebuild the header from the destination.
        sent = sendto(replaySocket6, packet + sizeof(struct ip6_hdr),
                      len - sizeof(struct ip6_hdr), MSG_DONTWAIT,
                      (struct sockaddr*)&dst6, sizeof(dst6));
    } else {
        const struct iphdr *ip = (const struct iphdr*)packet;
        struct sockaddr_in dst;

        if (replaySocket < 0) return 0;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family      = AF_INET;
        dst.sin_addr.s_addr = ip->daddr;
        sent = sendto(replaySocket, packet, len, MSG_DONTWAIT,
                      (struct sockaddr*)&dst, sizeof(dst));
    }

    if (sent < 0) {
        LOG("replay: sendto failed (%s)", strerror(errno));
        return 0;
    }
    return 1;
}

void packetBackendInjectClose(void) {
    if (replaySocket  >= 0) { close(replaySocket);  replaySocket  = -1; }
    if (replaySocket6 >= 0) { close(replaySocket6); replaySocket6 = -1; }
}

// ---------------------------------------------------------------------------
// Raw socket injection, used for synthetic packets only
// ---------------------------------------------------------------------------

// Injection is best-effort and MUST NOT block.
//
// This runs on the read/clock threads while they hold the capture mutex, so a
// blocking sendto() stalls the whole pipeline - and a stalled pipeline means
// divertStop() never completes. (Found the hard way: a full raw-socket send
// buffer wedged shutdown indefinitely.) The socket is therefore non-blocking
// and a would-block simply drops the clone, which is the right trade for a
// module whose entire purpose is generating redundant traffic.
static int injectRawPacket(PacketNode *node) {
    struct sockaddr_in dst;
    const struct iphdr *ip;
    ssize_t sent;

    if (node->packetLen < sizeof(struct iphdr)) return 0;
    if (rawSocket < 0 && rawSocket6 < 0) return 0;

    ip = (const struct iphdr*)node->packet;
    if (!node->meta.outbound) {
        // A raw socket can only originate traffic. Injecting a clone into the
        // local receive path would need a TUN device or an ifb redirect - out of
        // scope, and the original packet is still delivered normally.
        LOG("raw inject: inbound clone not supported, dropping");
        return 0;
    }

    if (injectDisabled) return 0;

    // Runaway detection. Legitimate use never approaches this rate: it means the
    // clones are being fed back into the queue, which only happens when the
    // fwmark bypass rule is missing.
    {
        const DWORD now = GetTickCount();
        if (injectWindowStart == 0 || (now - injectWindowStart) >= 1000) {
            injectWindowStart = now;
            injectWindowCount = 0;
        }
        if (++injectWindowCount > INJECT_RATE_LIMIT) {
            injectDisabled = 1;
            INFO("");
            INFO("  *** injection disabled: %d packets/s is a feedback loop ***",
                 INJECT_RATE_LIMIT);
            INFO("  Injected clones are re-entering your NFQUEUE rule and being");
            INFO("  cloned again. Add this rule BEFORE the NFQUEUE rule:");
            INFO("      sudo iptables -I OUTPUT -m mark --mark 0x%x -j ACCEPT", injectMark);
            INFO("  then restart the capture.");
            INFO("");
            return 0;
        }
    }

    if (ip->version == 6) {
        // IPv6 raw sockets do not support IP_HDRINCL, so the kernel builds the
        // header: hand it the payload after the 40-byte fixed header and let it
        // recreate the rest from the destination address.
        const struct ip6_hdr *ip6 = (const struct ip6_hdr*)node->packet;
        struct sockaddr_in6 dst6;

        if (rawSocket6 < 0 || node->packetLen <= sizeof(struct ip6_hdr)) {
            LOG("raw inject: no IPv6 socket, dropping clone");
            return 0;
        }
        memset(&dst6, 0, sizeof(dst6));
        dst6.sin6_family = AF_INET6;
        dst6.sin6_addr   = ip6->ip6_dst;

        sent = sendto(rawSocket6,
                      node->packet + sizeof(struct ip6_hdr),
                      node->packetLen - sizeof(struct ip6_hdr),
                      MSG_DONTWAIT,
                      (struct sockaddr*)&dst6, sizeof(dst6));
    } else {
        memset(&dst, 0, sizeof(dst));
        dst.sin_family      = AF_INET;
        dst.sin_addr.s_addr = ip->daddr;

        sent = sendto(rawSocket, node->packet, node->packetLen, MSG_DONTWAIT,
                      (struct sockaddr*)&dst, sizeof(dst));
    }
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
            // Kernel buffer is full - the clone is expendable, keep moving.
            ++rawDropCount;
            if ((rawDropCount & 0x3FF) == 1) {
                INFO("duplicate: raw socket saturated, %ld clone(s) dropped so far",
                     rawDropCount);
            }
        } else {
            LOG("raw inject failed: %s", strerror(errno));
        }
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Sending: drain the list, accepting survivors
// ---------------------------------------------------------------------------

static int sendAllListPackets() {
    int sendCount = 0;
    PacketNode *pnode;

    while (!isListEmpty()) {
        pnode = popNode(tail->prev);
        assert(pnode != head);

        // Phase 3.1: dump what actually leaves the machine, after every module.
        pcapExportWriteStage(PCAP_STAGE_POST, pnode->packet, pnode->packetLen,
                             pnode->meta.outbound);

        if (nodeMeta(pnode)->synthetic) {
            if (injectRawPacket(pnode)) {
                InterlockedExchange16(&sendState, SEND_STATUS_SEND);
            } else {
                InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
            }
        } else {
            settleVerdict(pnode, NF_ACCEPT);
            InterlockedExchange16(&sendState, SEND_STATUS_SEND);
        }

        freeNode(pnode);
        ++sendCount;
        InterlockedIncrement(&statsSentTotal);
    }
    assert(isListEmpty());
    return sendCount;
}

// ---------------------------------------------------------------------------
// Module pipeline - byte for byte the same logic as the Windows backend
// ---------------------------------------------------------------------------

static void divertConsumeStep() {
    int ix, cnt;

    if (pcapExportIsActive()) {
        PacketNode *pnode = head->next;
        while (pnode != tail) {
            pcapExportWriteStage(PCAP_STAGE_PRE, pnode->packet, pnode->packetLen,
                                 pnode->meta.outbound);
            pnode = pnode->next;
        }
    }

    for (ix = 0; ix < MODULE_CNT; ++ix) {
        Module *module = modules[ix];
        if (*(module->enabledFlag)) {
            if (!module->lastEnabled) {
                module->startUp();
                module->lastEnabled = 1;
            }
            if (module->process(head, tail)) {
                InterlockedIncrement16(&(module->processTriggered));
            }
        } else {
            if (module->lastEnabled) {
                module->closeDown(head, tail);
                module->lastEnabled = 0;
            }
        }
    }
    cnt = sendAllListPackets();
    UNREFERENCED_PARAMETER(cnt);
}

// ---------------------------------------------------------------------------
// NFQUEUE callback
// ---------------------------------------------------------------------------

static int nfqCallback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
                       struct nfq_data *nfa, void *cbData) {
    struct nfqnl_msg_packet_hdr *ph;
    unsigned char *payload = NULL;
    int len;
    UINT32 id;
    PacketMeta meta;
    LinuxPacketMeta backendMeta;
    PacketNode *pnode;

    UNREFERENCED_PARAMETER(nfmsg);
    UNREFERENCED_PARAMETER(cbData);

    ph = nfq_get_msg_packet_hdr(nfa);
    if (!ph) return 0;
    id = ntohl(ph->packet_id);

    len = nfq_get_payload(nfa, &payload);
    if (len <= 0 || payload == NULL) {
        nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
        return 0;
    }

    memset(&meta, 0, sizeof(meta));
    // NFQUEUE has no "direction" bit. An outbound hook (OUTPUT/POSTROUTING) has
    // no inbound interface index, and an inbound hook has no outbound one, so
    // that is what we key on - the same signal iptables itself uses.
    meta.outbound  = (unsigned char)(nfq_get_indev(nfa) == 0 ? 1 : 0);
    meta.ipVersion = (unsigned char)(((payload[0] >> 4) == 6) ? 6 : 4);
    meta.ifIdx     = meta.outbound ? nfq_get_outdev(nfa) : nfq_get_indev(nfa);
    meta.subIfIdx  = 0;
    // Loopback shows up as the lo interface on both physical dev queries.
    meta.loopback  = (unsigned char)(nfq_get_physindev(nfa) == 1 ||
                                     nfq_get_physoutdev(nfa) == 1 ? 1 : 0);

    // Phase 4.3: iptables decided what reaches this queue; the clumsy filter
    // expression decides what clumsy actually messes with. Anything that does
    // not match is accepted straight through, untouched.
    if (filterProg && !filterMatch(filterProg, &meta, (const char*)payload, (UINT)len)) {
        nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
        return 0;
    }

    memset(&backendMeta, 0, sizeof(backendMeta));
    backendMeta.packetId = id;

    InterlockedIncrement(&statsCapturedTotal);
    pnode = createNode((char*)payload, (UINT)len, &meta, &backendMeta);
    appendNode(pnode);
    divertConsumeStep();
    return 0;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

static DWORD divertReadLoop(LPVOID arg) {
    // Aligned so the netlink header inside is naturally aligned.
    char buf[MAX_PACKETSIZE] __attribute__((aligned(8)));
    DWORD waitResult;

    UNREFERENCED_PARAMETER(arg);

    for (;;) {
        struct pollfd pfd;
        int rv;

        pfd.fd      = nfqFd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        // Poll rather than block so stopLooping is noticed promptly.
        rv = poll(&pfd, 1, 50);
        if (stopLooping) {
            LOG("Read stopLooping, exiting read loop.");
            return 0;
        }
        if (rv <= 0) continue;

        rv = (int)recv(nfqFd, buf, sizeof(buf), 0);
        if (rv < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == ENOBUFS) {
                // Kernel dropped packets because we were too slow. Non-fatal.
                LOG("nfq: ENOBUFS, kernel dropped packets");
                continue;
            }
            LOG("nfq recv failed: %s", strerror(errno));
            return 0;
        }

        waitResult = WaitForSingleObject(mutex, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            LOG("Acquire failed/abandoned.");
            return 0;
        }
        /***************** enter critical region ************************/
        if (stopLooping) {
            ReleaseMutex(mutex);
            return 0;
        }
        // nfq_handle_packet dispatches into nfqCallback, which appends to the
        // list and runs the module pipeline - all under this mutex.
        nfq_handle_packet(nfqHandle, buf, rv);
        /***************** leave critical region ************************/
        if (!ReleaseMutex(mutex)) {
            LOG("Fatal: Failed to release mutex");
            ABORT();
        }
    }
}

static DWORD divertClockLoop(LPVOID arg) {
    DWORD startTick, stepTick, waitResult;
    int ix;

    UNREFERENCED_PARAMETER(arg);

    for (;;) {
        startTick = GetTickCount();
        waitResult = WaitForSingleObject(mutex, CLOCK_WAITMS);
        switch (waitResult) {
        case WAIT_OBJECT_0:
            /***************** enter critical region ************************/
            divertConsumeStep();
            /***************** leave critical region ************************/
            if (!ReleaseMutex(mutex)) {
                InterlockedIncrement16(&stopLooping);
                LOG("Fatal: Failed to release mutex");
                ABORT();
            }
            stepTick = GetTickCount() - startTick;
            if (stepTick < CLOCK_WAITMS) Sleep(CLOCK_WAITMS - stepTick);
            break;
        case WAIT_TIMEOUT:
            LOG("!!! Skipping one run");
            Sleep(CLOCK_WAITMS);
            break;
        default:
            LOG("Acquire failed/abandoned");
            InterlockedIncrement16(&stopLooping);
            break;
        }

        if (stopLooping) {
            waitResult = WaitForSingleObject(mutex, INFINITE);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
                /***************** enter critical region ************************/
                LOG("Read stopLooping, stopping...");
                for (ix = 0; ix < MODULE_CNT; ++ix) {
                    Module *module = modules[ix];
                    if (*(module->enabledFlag)) module->closeDown(head, tail);
                }
                LOG("Send all packets upon closing");
                sendAllListPackets();
                /***************** leave critical region ************************/
                ReleaseMutex(mutex);
            }
            return 0;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API - same signatures as the Windows backend
// ---------------------------------------------------------------------------

int divertStart(const char *filter, char buf[]) {
    int ix;
    int one = 1;
    char filterErr[MSG_BUFSIZE] = "";

    queueNum = argGetInt("queue-num", 0);

    // Phase 4.3: compile the clumsy filter expression up front so a syntax
    // error is reported at Start, exactly like WinDivertOpen does on Windows.
    filterProg = filterCompile(filter, filterErr, sizeof(filterErr));
    if (!filterProg) {
        snprintf(buf, MSG_BUFSIZE, "Failed to start filtering : %s", filterErr);
        return FALSE;
    }

    nfqHandle = nfq_open();
    if (!nfqHandle) {
        snprintf(buf, MSG_BUFSIZE,
                 "Failed to open NFQUEUE (%s).\n"
                 "Make sure you run clumsy as root (or with CAP_NET_ADMIN).",
                 strerror(errno));
        filterFree(filterProg); filterProg = NULL;
        return FALSE;
    }

    // Legacy unbind/bind: no-ops on modern kernels, needed on very old ones.
    nfq_unbind_pf(nfqHandle, AF_INET);
    if (nfq_bind_pf(nfqHandle, AF_INET) < 0) {
        snprintf(buf, MSG_BUFSIZE, "nfq_bind_pf(AF_INET) failed (%s)", strerror(errno));
        goto fail;
    }
    nfq_bind_pf(nfqHandle, AF_INET6);

    nfqQueue = nfq_create_queue(nfqHandle, (UINT16)queueNum, &nfqCallback, NULL);
    if (!nfqQueue) {
        snprintf(buf, MSG_BUFSIZE,
                 "Failed to bind queue %d (%s).\n"
                 "Another process may already own it - try --queue-num N.",
                 queueNum, strerror(errno));
        goto fail;
    }

    if (nfq_set_mode(nfqQueue, NFQNL_COPY_PACKET, MAX_PACKETSIZE) < 0) {
        snprintf(buf, MSG_BUFSIZE, "nfq_set_mode failed (%s)", strerror(errno));
        goto fail;
    }
    // clumsy legitimately holds thousands of packets (lag, bandwidth), so the
    // default queue length of 1024 is far too small.
    nfq_set_queue_maxlen(nfqQueue, NFQ_QUEUE_MAXLEN);

    nfqFd = nfq_fd(nfqHandle);
    nfnl_rcvbufsiz(nfq_nfnlh(nfqHandle), NFQ_RECV_BUFSIZE);

    // Raw socket for synthetic packets (duplicate module). Optional: without it
    // clones are dropped and everything else still works.
    // SOCK_NONBLOCK is essential here, see injectRawPacket().
    injectMark = (UINT32)argGetInt("inject-mark", DEFAULT_INJECT_MARK);
    rawSocket = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_RAW);
    if (rawSocket >= 0) {
        int sndbuf = 1 << 20;
        setsockopt(rawSocket, IPPROTO_IP, IP_HDRINCL, &one, sizeof(one));
        setsockopt(rawSocket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        // Stamps skb->mark so an iptables rule can let our own injections past
        // the NFQUEUE rule instead of feeding them back in.
        if (setsockopt(rawSocket, SOL_SOCKET, SO_MARK, &injectMark, sizeof(injectMark)) < 0) {
            INFO("warning: cannot set SO_MARK on the injection socket (%s); the "
                 "duplicate module may loop.", strerror(errno));
        }
        rawDropCount = 0;
        injectWindowCount = 0;
        injectWindowStart = 0;
        injectDisabled = 0;

        // Companion socket for IPv6 clones. Optional: without it only IPv6
        // clones are lost, everything else keeps working.
        rawSocket6 = socket(AF_INET6, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_RAW);
        if (rawSocket6 >= 0) {
            setsockopt(rawSocket6, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
            setsockopt(rawSocket6, SOL_SOCKET, SO_MARK, &injectMark, sizeof(injectMark));
        } else {
            LOG("no IPv6 injection socket (%s); IPv6 clones will be dropped",
                strerror(errno));
        }
    } else {
        INFO("warning: raw socket unavailable (%s); the duplicate module cannot "
             "inject clones.", strerror(errno));
    }

    initPacketNodeList();
    for (ix = 0; ix < MODULE_CNT; ++ix) modules[ix]->lastEnabled = 0;

    stopLooping = FALSE;
    mutex = CreateMutex(NULL, FALSE, NULL);
    if (mutex == NULL) {
        snprintf(buf, MSG_BUFSIZE, "Failed to create mutex");
        goto fail;
    }
    loopThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)divertReadLoop, NULL, 0, NULL);
    if (loopThread == NULL) {
        snprintf(buf, MSG_BUFSIZE, "Failed to create recv loop thread");
        goto fail;
    }
    clockThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)divertClockLoop, NULL, 0, NULL);
    if (clockThread == NULL) {
        snprintf(buf, MSG_BUFSIZE, "Failed to create clock loop thread");
        goto fail;
    }

    if (parseBoolValue(argGet("auto-iptables"))) {
        if (!iptablesAutoInstall(filterProg, queueNum, injectMark)) {
            snprintf(buf, MSG_BUFSIZE,
                     "Failed to install iptables rules. Run as root, or drop "
                     "--auto-iptables and add the rules manually.");
            // Unwind everything divertStart set up so far.
            InterlockedIncrement16(&stopLooping);
            WaitForMultipleObjects(2, (HANDLE[]){loopThread, clockThread}, TRUE, 3000);
            CloseHandle(loopThread); CloseHandle(clockThread);
            loopThread = clockThread = NULL;
            goto fail;
        }
    } else {
        INFO("NFQUEUE bound to queue %d. Feed it with rules like these, in order:", queueNum);
        INFO("  sudo iptables -I OUTPUT -m mark --mark 0x%x -j ACCEPT", injectMark);
        INFO("  sudo iptables -A OUTPUT -p udp --dport 9999 -j NFQUEUE --queue-num %d --queue-bypass", queueNum);
        INFO("  The first rule is required whenever the duplicate module is used: it");
        INFO("  stops clumsy's own injected packets from re-entering the queue.");
        INFO("  Remove both with -D when you are done.");
        INFO("  (or pass --auto-iptables and let clumsy manage them for you)");
    }
    return TRUE;

fail:
    if (nfqQueue)  { nfq_destroy_queue(nfqQueue); nfqQueue = NULL; }
    if (nfqHandle) { nfq_close(nfqHandle); nfqHandle = NULL; }
    if (rawSocket  >= 0) { close(rawSocket);  rawSocket  = -1; }
    if (rawSocket6 >= 0) { close(rawSocket6); rawSocket6 = -1; }
    if (filterProg) { filterFree(filterProg); filterProg = NULL; }
    return FALSE;
}

void divertStop() {
    HANDLE threads[2];
    threads[0] = loopThread;
    threads[1] = clockThread;

    LOG("Stopping...");
    InterlockedIncrement16(&stopLooping);
    // Bounded rather than INFINITE: both loops are designed to notice
    // stopLooping within ~50ms, so anything longer means a backend call is
    // wedged. Say so instead of hanging the process with no explanation.
    if (WaitForMultipleObjects(2, threads, TRUE, 5000) != WAIT_OBJECT_0) {
        INFO("warning: capture threads did not stop within 5s; leaking them and "
             "continuing shutdown.");
        // Deliberately not CloseHandle: a still-running thread would write into
        // freed handle memory. The iptables rules still come out - leaving them
        // installed is far worse than leaking two threads on the way out.
        loopThread = clockThread = NULL;
        iptablesAutoRemove();
        if (rawSocket  >= 0) { close(rawSocket);  rawSocket  = -1; }
        if (rawSocket6 >= 0) { close(rawSocket6); rawSocket6 = -1; }
        return;
    }
    CloseHandle(loopThread);
    CloseHandle(clockThread);
    loopThread = clockThread = NULL;

    // Rules come out before the queue does: the reverse order would leave a
    // brief window where the rule points at a queue nobody is reading.
    iptablesAutoRemove();

    if (nfqQueue)  { nfq_destroy_queue(nfqQueue); nfqQueue = NULL; }
    if (nfqHandle) { nfq_close(nfqHandle); nfqHandle = NULL; }
    if (rawSocket  >= 0) { close(rawSocket);  rawSocket  = -1; }
    if (rawSocket6 >= 0) { close(rawSocket6); rawSocket6 = -1; }
    if (mutex) { CloseHandle(mutex); mutex = NULL; }
    if (filterProg) { filterFree(filterProg); filterProg = NULL; }
    nfqFd = -1;

    LOG("Successfully waited threads and stopped.");
}

void statsReset(void) {
    int ix;
    InterlockedExchange(&statsCapturedTotal, 0);
    InterlockedExchange(&statsSentTotal, 0);
    // The delay histogram belongs to the session too; carrying it across a
    // restart would blend two different module configurations into one curve.
    latencyReset();
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        InterlockedExchange(&(modules[ix]->affectedCount), 0);
    }
}
