#include <stdlib.h>
#include <memory.h>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include "windivert.h"
#include "common.h"
#define DIVERT_PRIORITY 0
#define MAX_PACKETSIZE 0xFFFF
#define READ_TIME_PER_STEP 3
// FIXME does this need to be larger then the time to process the list?
#define CLOCK_WAITMS 40
#define QUEUE_LEN 2 << 10
#define QUEUE_TIME 2 << 9 

static HANDLE divertHandle;
static volatile short stopLooping;
static HANDLE loopThread, clockThread, mutex;

// global stats counters
volatile LONG statsCapturedTotal = 0;
volatile LONG statsSentTotal = 0;

static DWORD divertReadLoop(LPVOID arg);
static DWORD divertClockLoop(LPVOID arg);

// Phase 4.1: this file is the Windows capture backend, so it is one of the two
// places allowed to see WINDIVERT_ADDRESS. Everything downstream sees only
// PacketMeta plus the opaque blob carried in PacketNode::backend.
static_assert(sizeof(WINDIVERT_ADDRESS) <= PACKET_BACKEND_META_SIZE,
              "PACKET_BACKEND_META_SIZE is too small for WINDIVERT_ADDRESS");

static void fillMetaFromAddr(PacketMeta *meta, const WINDIVERT_ADDRESS *addr) {
    meta->outbound  = (unsigned char)(addr->Outbound ? 1 : 0);
    meta->loopback  = (unsigned char)(addr->Loopback ? 1 : 0);
    meta->impostor  = (unsigned char)(addr->Impostor ? 1 : 0);
    meta->ipVersion = (unsigned char)(addr->IPv6 ? 6 : 4);
    meta->ifIdx     = addr->Network.IfIdx;
    meta->subIfIdx  = addr->Network.SubIfIdx;
}

// The WinDivert address a node was captured with, needed to re-inject it.
static INLINE_FUNCTION PWINDIVERT_ADDRESS nodeAddr(PacketNode *node) {
    return (PWINDIVERT_ADDRESS)node->backend.raw;
}

// --- capture backend hooks (Phase 4.2) ---
// On Windows a dropped packet is simply one that is never sent back, so there
// is nothing to settle. NFQUEUE is the one that has to answer for every id.
void packetBackendOnFree(PacketNode *node) {
    UNREFERENCED_PARAMETER(node);
}

// A clone re-injects through the same address as the original.
void packetBackendPrepareClone(const PacketNode *src, PacketNode *dst) {
    memcpy(dst->backend.raw, src->backend.raw, PACKET_BACKEND_META_SIZE);
}

// --- replay injection (T7) ---
//
// pcap replay has no captured WINDIVERT_ADDRESS to reuse, so it needs its own
// send-only handle and a synthesised address. The handle is opened with the
// filter "false" - it must never receive anything, only send - which is the
// documented way to get a WinDivert handle purely for injection.
//
// Opened lazily on the replay thread, which is its only caller. That thread
// also closes it on the way out, so no lock is needed here.
static HANDLE injectHandle = INVALID_HANDLE_VALUE;
// Set once WinDivertOpen has failed. Without it a replay started without
// Administrator rights retries the open - and repeats the same message - for
// every one of a capture file's millions of records.
static int injectOpenFailed = 0;

int packetBackendInject(char *packet, UINT len, BOOL outbound) {
    WINDIVERT_ADDRESS addr;
    UINT sendLen = 0;
    int isIPv6;

    if (!packet || len < sizeof(WINDIVERT_IPHDR)) return 0;

    // The IP version comes from the packet itself; the caller only knows the
    // direction it wants. Check the length against the header the packet
    // actually claims to carry - a truncated record whose first nibble is 6
    // would otherwise clear the 20-byte IPv4 gate and reach WinDivert with
    // less than a full IPv6 header, which the Linux backend rejects outright.
    isIPv6 = (((unsigned char)packet[0] >> 4) == 6) ? 1 : 0;
    if (isIPv6 && len < sizeof(WINDIVERT_IPV6HDR)) return 0;

    if (injectHandle == INVALID_HANDLE_VALUE) {
        if (injectOpenFailed) return 0;
        injectHandle = WinDivertOpen("false", WINDIVERT_LAYER_NETWORK, 0,
                                     WINDIVERT_FLAG_SEND_ONLY);
        if (injectHandle == INVALID_HANDLE_VALUE) {
            INFO("replay: cannot open a WinDivert injection handle (%lu). "
                 "Administrator rights are required.", GetLastError());
            injectOpenFailed = 1;
            return 0;
        }
        LOG("replay: opened send-only WinDivert handle");
    }

    memset(&addr, 0, sizeof(addr));
    addr.Layer    = WINDIVERT_LAYER_NETWORK;
    addr.Event    = WINDIVERT_EVENT_NETWORK_PACKET;
    addr.Outbound = outbound ? 1 : 0;
    addr.IPv6     = isIPv6;
    // Marking the packet as an impostor is what lets a running capture tell
    // replayed traffic apart from live traffic ("not impostor" in a filter).
    addr.Impostor = 1;

    // Recomputes whatever the recorded packet needs and sets the matching
    // valid-checksum flags on addr.
    WinDivertHelperCalcChecksums(packet, len, &addr, 0);

    if (!WinDivertSend(injectHandle, packet, len, &sendLen, &addr)) {
        LOG("replay: WinDivertSend failed (%lu)", GetLastError());
        return 0;
    }
    return (sendLen >= len) ? 1 : 0;
}

void packetBackendInjectClose(void) {
    if (injectHandle != INVALID_HANDLE_VALUE) {
        WinDivertClose(injectHandle);
        injectHandle = INVALID_HANDLE_VALUE;
        LOG("replay: closed injection handle");
    }
    // Clear the sticky failure too: the next run may well be the elevated one.
    injectOpenFailed = 0;
}

// not to put these in common.h since modules shouldn't see these
extern PacketNode * const head;
extern PacketNode * const tail;

#ifdef _DEBUG
PWINDIVERT_IPHDR dbg_ip_header;
PWINDIVERT_IPV6HDR dbg_ipv6_header;
PWINDIVERT_TCPHDR dbg_tcp_header;
PWINDIVERT_UDPHDR dbg_udp_header;
PWINDIVERT_ICMPHDR dbg_icmp_header;
PWINDIVERT_ICMPV6HDR dbg_icmpv6_header;
UINT payload_len;
void dumpPacket(char *buf, int len, PWINDIVERT_ADDRESS paddr) {
    const char *protocol;
    UINT16 srcPort = 0, dstPort = 0;

    WinDivertHelperParsePacket(buf, len, &dbg_ip_header, &dbg_ipv6_header, NULL,
        &dbg_icmp_header, &dbg_icmpv6_header, &dbg_tcp_header, &dbg_udp_header,
        NULL, &payload_len, NULL, NULL);
    // need to cast byte order on port numbers
    if (dbg_tcp_header != NULL) {
        protocol = "TCP ";
        srcPort = ntohs(dbg_tcp_header->SrcPort);
        dstPort = ntohs(dbg_tcp_header->DstPort);
    } else if (dbg_udp_header != NULL) {
        protocol = "UDP ";
        srcPort = ntohs(dbg_udp_header->SrcPort);
        dstPort = ntohs(dbg_udp_header->DstPort);
    } else if (dbg_icmp_header || dbg_icmpv6_header) {
        protocol = "ICMP";
    } else {
        protocol = "???";
    }

    if (dbg_ip_header != NULL) {
        UINT8 *src_addr = (UINT8*)&dbg_ip_header->SrcAddr;
        UINT8 *dst_addr = (UINT8*)&dbg_ip_header->DstAddr;
        LOG("%s.%s: %u.%u.%u.%u:%d->%u.%u.%u.%u:%d",
            protocol,
            paddr->Outbound ? "OUT " : "IN  ",
            src_addr[0], src_addr[1], src_addr[2], src_addr[3], srcPort,
            dst_addr[0], dst_addr[1], dst_addr[2], dst_addr[3], dstPort);
    } else if (dbg_ipv6_header != NULL) {
        UINT16 *src_addr6 = (UINT16*)&dbg_ipv6_header->SrcAddr;
        UINT16 *dst_addr6 = (UINT16*)&dbg_ipv6_header->DstAddr;
        LOG("%s.%s: %x:%x:%x:%x:%x:%x:%x:%x:%d->%x:%x:%x:%x:%x:%x:%x:%x:%d",
            protocol,
            paddr->Outbound ? "OUT " : "IN  ",
            src_addr6[0], src_addr6[1], src_addr6[2], src_addr6[3],
            src_addr6[4], src_addr6[5], src_addr6[6], src_addr6[7], srcPort,
            dst_addr6[0], dst_addr6[1], dst_addr6[2], dst_addr6[3],
            dst_addr6[4], dst_addr6[5], dst_addr6[6], dst_addr6[7], dstPort);
    }
}
#else
#define dumpPacket(x, y, z)
#endif

int divertStart(const char *filter, char buf[]) {
    int ix;

    divertHandle = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, DIVERT_PRIORITY, 0);
    if (divertHandle == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_INVALID_PARAMETER) {
            strcpy(buf, "Failed to start filtering : filter syntax error.");
        } else {
            sprintf(buf, "Failed to start filtering : failed to open device (code:%lu).\n"
                "Make sure you run clumsy as Administrator.", lastError);
        }
        return FALSE;
    }
    LOG("Divert opened handle.");

    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_LENGTH, QUEUE_LEN);
    WinDivertSetParam(divertHandle, WINDIVERT_PARAM_QUEUE_TIME, QUEUE_TIME);
    LOG("WinDivert internal queue Len: %d, queue time: %d", QUEUE_LEN, QUEUE_TIME);

    // init package link list
    initPacketNodeList();

    // reset module
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        modules[ix]->lastEnabled = 0;
    }

    // kick off the loop
    LOG("Creating threads and mutex...");
    stopLooping = FALSE;
    mutex = CreateMutex(NULL, FALSE, NULL);
    if (mutex == NULL) {
        sprintf(buf, "Failed to create mutex (%lu)", GetLastError());
        return FALSE;
    }

    loopThread = CreateThread(NULL, 1, (LPTHREAD_START_ROUTINE)divertReadLoop, NULL, 0, NULL);
    if (loopThread == NULL) {
        sprintf(buf, "Failed to create recv loop thread (%lu)", GetLastError());
        return FALSE;
    }
    clockThread = CreateThread(NULL, 1, (LPTHREAD_START_ROUTINE)divertClockLoop, NULL, 0, NULL);
    if (clockThread == NULL) {
        sprintf(buf, "Failed to create clock loop thread (%lu)", GetLastError());
        return FALSE;
    }

    LOG("Threads created");

    return TRUE;
}

static int sendAllListPackets() {
    // send packet from tail to head and remove sent ones
    int sendCount = 0;
    UINT sendLen;
    PacketNode *pnode;
#ifdef _DEBUG
    // check the list is good
    // might go into dead loop but it's better for debugging
    PacketNode *p = head;
    do {
        p = p->next;
    } while (p->next);
    assert(p == tail);
#endif

    while (!isListEmpty()) {
        PWINDIVERT_ADDRESS paddr;
        pnode = popNode(tail->prev);
        paddr = nodeAddr(pnode);
        sendLen = 0;
        assert(pnode != head);
        // Phase 3.1: dump what actually leaves the machine, after every module.
        pcapExportWriteStage(PCAP_STAGE_POST, pnode->packet, pnode->packetLen,
                             pnode->meta.outbound);
        // FIXME inbound injection on any kind of packet is failing with a very high percentage
        //       need to contact windivert auther and wait for next release
        if (!WinDivertSend(divertHandle, pnode->packet, pnode->packetLen, &sendLen, paddr)) {
            PWINDIVERT_ICMPHDR icmp_header;
            PWINDIVERT_ICMPV6HDR icmpv6_header;
            PWINDIVERT_IPHDR ip_header;
            PWINDIVERT_IPV6HDR ipv6_header;
            LOG("Failed to send a packet. (%lu)", GetLastError());
            dumpPacket(pnode->packet, pnode->packetLen, paddr);
            // as noted in windivert help, reinject inbound icmp packets some times would fail
            // workaround this by resend them as outbound
            // TODO not sure is this even working as can't find a way to test
            //      need to document about this
            WinDivertHelperParsePacket(pnode->packet, pnode->packetLen, &ip_header, &ipv6_header, NULL,
                &icmp_header, &icmpv6_header, NULL, NULL, NULL, NULL, NULL, NULL);
            if ((icmp_header || icmpv6_header) && !paddr->Outbound) {
                BOOL resent;
                paddr->Outbound = TRUE;
                pnode->meta.outbound = 1;   // keep the neutral view in sync
                if (ip_header) {
                    UINT32 tmp = ip_header->SrcAddr;
                    ip_header->SrcAddr = ip_header->DstAddr;
                    ip_header->DstAddr = tmp;
                } else if (ipv6_header) {
                    UINT32 tmpArr[4];
                    memcpy(tmpArr, ipv6_header->SrcAddr, sizeof(tmpArr));
                    memcpy(ipv6_header->SrcAddr, ipv6_header->DstAddr, sizeof(tmpArr));
                    memcpy(ipv6_header->DstAddr, tmpArr, sizeof(tmpArr));
                }
                resent = WinDivertSend(divertHandle, pnode->packet, pnode->packetLen, &sendLen, paddr);
                LOG("Resend failed inbound ICMP packets as outbound: %s", resent ? "SUCCESS" : "FAIL");
                InterlockedExchange16(&sendState, SEND_STATUS_SEND);
            } else {
                InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
            }
        } else {
            if (sendLen < pnode->packetLen) {
                // TODO don't know how this can happen, or it needs to be resent like good old UDP packet
                LOG("Internal Error: DivertSend truncated send packet.");
                InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
            } else {
                InterlockedExchange16(&sendState, SEND_STATUS_SEND);
            }
        }


        freeNode(pnode);
        ++sendCount;
        InterlockedIncrement(&statsSentTotal);
    }
    assert(isListEmpty()); // all packets should be sent by now

    return sendCount;
}

// step function to let module process and consume all packets on the list
static void divertConsumeStep() {
#ifdef _DEBUG
    DWORD startTick = GetTickCount(), dt;
#endif
    int ix, cnt;

    // Phase 3.1: dump the packets as captured, before any module touched them.
    // Cheap when pcap is off — pcapExportWriteStage() returns on a NULL file.
    if (pcapExportIsActive()) {
        PacketNode *pnode = head->next;
        while (pnode != tail) {
            pcapExportWriteStage(PCAP_STAGE_PRE, pnode->packet, pnode->packetLen,
                                 pnode->meta.outbound);
            pnode = pnode->next;
        }
    }

    // use lastEnabled to keep track of module starting up and closing down
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
#ifdef _DEBUG
    dt =  GetTickCount() - startTick;
    if (dt > CLOCK_WAITMS / 2) {
        LOG("Costy consume step: %lu ms, sent %d packets", GetTickCount() - startTick, cnt);
    }
#endif
}

// periodically try to consume packets to keep the network responsive and not blocked by recv
static DWORD divertClockLoop(LPVOID arg) {
    DWORD startTick, stepTick, waitResult;
    int ix;

    UNREFERENCED_PARAMETER(arg);

    for(;;) {
        // use acquire as wait for yielding thread
        startTick = GetTickCount();
        waitResult = WaitForSingleObject(mutex, CLOCK_WAITMS);
        switch(waitResult) {
            case WAIT_OBJECT_0:
                /***************** enter critical region ************************/
                divertConsumeStep();
                /***************** leave critical region ************************/
                if (!ReleaseMutex(mutex)) {
                    InterlockedIncrement16(&stopLooping);
                    LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                    ABORT();
                }
                // if didn't spent enough time, we sleep on it
                stepTick = GetTickCount() - startTick;
                if (stepTick < CLOCK_WAITMS) {
                    Sleep(CLOCK_WAITMS - stepTick);
                }
                break;
            case WAIT_TIMEOUT:
                // read loop is processing, so we can skip this run
                LOG("!!! Skipping one run");
                Sleep(CLOCK_WAITMS);
                break;
            case WAIT_ABANDONED:
                LOG("Acquired abandoned mutex");
                InterlockedIncrement16(&stopLooping);
                break;
            case WAIT_FAILED:
                LOG("Acquire failed (%lu)", GetLastError());
                InterlockedIncrement16(&stopLooping);
                break;
        }

        // need to get the lock here
        if (stopLooping) {
            int lastSendCount = 0;
            BOOL closed;

            waitResult = WaitForSingleObject(mutex, INFINITE);
            switch (waitResult)
            {
            case WAIT_ABANDONED:
            case WAIT_FAILED:
                LOG("Acquire failed/abandoned mutex (%lu), will still try closing and return", GetLastError());
            case WAIT_OBJECT_0:
                /***************** enter critical region ************************/
                LOG("Read stopLooping, stopping...");
                // clean up by closing all modules
                for (ix = 0; ix < MODULE_CNT; ++ix) {
                    Module *module = modules[ix];
                    if (*(module->enabledFlag)) {
                        module->closeDown(head, tail);
                    } 
                }
                LOG("Send all packets upon closing");
                lastSendCount = sendAllListPackets();
                LOG("Lastly sent %d packets. Closing...", lastSendCount);

                // terminate recv loop by closing handler. handle related error in recv loop to quit
                closed = WinDivertClose(divertHandle);
                assert(closed);

                // release to let read loop exit properly
                /***************** leave critical region ************************/
                if (!ReleaseMutex(mutex)) {
                    LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                    ABORT();
                }
                return 0;
                break;
            }
        }
    }
}

static DWORD divertReadLoop(LPVOID arg) {
    char packetBuf[MAX_PACKETSIZE];
    WINDIVERT_ADDRESS addrBuf;
    PacketMeta metaBuf;
    UINT readLen;
    PacketNode *pnode;
    DWORD waitResult;

    UNREFERENCED_PARAMETER(arg);

    for(;;) {
        // each step must fully consume the list
        assert(isListEmpty()); // FIXME has failed this assert before. don't know why
        if (!WinDivertRecv(divertHandle, packetBuf, MAX_PACKETSIZE, &readLen, &addrBuf)) {
            DWORD lastError = GetLastError();
            if (lastError == ERROR_INVALID_HANDLE || lastError == ERROR_OPERATION_ABORTED) {
                // treat closing handle as quit
                LOG("Handle died or operation aborted. Exit loop.");
                return 0;
            }
            LOG("Failed to recv a packet. (%lu)", GetLastError());
            continue;
        }
        if (readLen > MAX_PACKETSIZE) {
            // don't know how this can happen
            LOG("Internal Error: DivertRecv truncated recv packet."); 
        }

        //dumpPacket(packetBuf, readLen, &addrBuf);  

        waitResult = WaitForSingleObject(mutex, INFINITE);
        switch(waitResult) {
            case WAIT_OBJECT_0:
                /***************** enter critical region ************************/
                if (stopLooping) {
                    LOG("Lost last recved packet but user stopped. Stop read loop.");
                    /***************** leave critical region ************************/
                    if (!ReleaseMutex(mutex)) {
                        LOG("Fatal: Failed to release mutex on stopping (%lu). Will stop anyway.", GetLastError());
                    }
                    return 0;
                }
                // create node and put it into the list
                fillMetaFromAddr(&metaBuf, &addrBuf);
                pnode = createNode(packetBuf, readLen, &metaBuf, &addrBuf);
                if (pnode) {
                    InterlockedIncrement(&statsCapturedTotal);
                    appendNode(pnode);
                    divertConsumeStep();
                } else {
                    // Out of memory. On Windows a packet clumsy never sends
                    // back is a dropped packet, so there is nothing else to
                    // settle - just do not follow the NULL.
                    LOG("Out of memory, dropping one captured packet");
                }
                /***************** leave critical region ************************/
                if (!ReleaseMutex(mutex)) {
                    LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                    ABORT();
                }
                break;
            case WAIT_TIMEOUT:
                LOG("Acquire timeout, dropping one read packet");
                continue;
                break;
            case WAIT_ABANDONED:
                LOG("Acquire abandoned.");
                return 0;
            case WAIT_FAILED:
                LOG("Acquire failed.");
                return 0;
        }
    }
}

void statsReset(void) {
    int ix;
    InterlockedExchange(&statsCapturedTotal, 0);
    InterlockedExchange(&statsSentTotal, 0);
    // The delay histogram belongs to the session too; carrying it across a
    // restart would blend two different module configurations into one curve.
    latencyReset();
    // Same reasoning for the one module that reports more than a packet count.
    corruptResetStats();
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        InterlockedExchange(&(modules[ix]->affectedCount), 0);
    }
}

void divertStop() {
    HANDLE threads[2];
    threads[0] = loopThread;
    threads[1] = clockThread;

    LOG("Stopping...");
    InterlockedIncrement16(&stopLooping);
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);

    LOG("Successfully waited threads and stopped.");
}
