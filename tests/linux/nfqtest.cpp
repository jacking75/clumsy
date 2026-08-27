// nfqtest — minimal NFQUEUE capability probe for clumsy Phase 4.
//
// Proves, on this exact WSL2 kernel, the four things divert_linux.cpp will need:
//   accept  — see every packet and let it through          (= no module enabled)
//   drop    — discard a packet                             (= drop / blackout)
//   delay   — hold a packet and issue the verdict later    (= lag / jitter / throttle)
//   mangle  — rewrite the payload and re-checksum          (= tamper / reset)
//
// Built with g++-16 -std=c++23 to also exercise the toolchain end to end.

// Include order matters here, and Phase 4 will hit the same two traps:
//
//  1. The glibc network headers must come BEFORE <linux/netfilter.h>.
//     linux/in.h and netinet/in.h both define struct in_addr; linux/libc-compat.h
//     only suppresses the kernel copies when it can see glibc arrived first.
//  2. NF_ACCEPT / NF_DROP live in <linux/netfilter.h>, not in any
//     libnetfilter_queue header.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <print>
#include <string>

namespace {

enum class Mode { Accept, Drop, Delay, Mangle };

Mode        g_mode      = Mode::Accept;
int         g_delayMs   = 300;
int         g_seen      = 0;
nfq_q_handle *g_qh      = nullptr;

using Clock = std::chrono::steady_clock;

// Packets whose verdict is deliberately postponed (the lag module's core trick).
struct Held {
    std::uint32_t id;
    Clock::time_point due;
};
std::deque<Held> g_held;

const char* modeName(Mode m) {
    switch (m) {
    case Mode::Drop:   return "drop";
    case Mode::Delay:  return "delay";
    case Mode::Mangle: return "mangle";
    default:           return "accept";
    }
}

// Overwrite the UDP payload in place. For IPv4/UDP a zero checksum means
// "not computed", which is legal and keeps this probe short — clumsy itself
// will recompute properly.
bool manglePayload(unsigned char *pkt, int len) {
    if (len < static_cast<int>(sizeof(iphdr))) return false;
    auto *ip = reinterpret_cast<iphdr *>(pkt);
    if (ip->protocol != IPPROTO_UDP) return false;

    const int ipHdrLen = ip->ihl * 4;
    if (len < ipHdrLen + static_cast<int>(sizeof(udphdr))) return false;

    auto *udp = reinterpret_cast<udphdr *>(pkt + ipHdrLen);
    unsigned char *data = pkt + ipHdrLen + sizeof(udphdr);
    const int dataLen = len - ipHdrLen - static_cast<int>(sizeof(udphdr));
    if (dataLen <= 0) return false;

    for (int i = 0; i < dataLen; ++i) data[i] = 'X';
    udp->check = 0;
    return true;
}

int callback(nfq_q_handle *qh, nfgenmsg *, nfq_data *nfa, void *) {
    nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfa);
    const std::uint32_t id = ph ? ntohl(ph->packet_id) : 0;

    unsigned char *payload = nullptr;
    const int len = nfq_get_payload(nfa, &payload);
    ++g_seen;

    std::string where = "?";
    if (len >= static_cast<int>(sizeof(iphdr)) && payload) {
        auto *ip = reinterpret_cast<iphdr *>(payload);
        char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip->saddr, src, sizeof(src));
        inet_ntop(AF_INET, &ip->daddr, dst, sizeof(dst));
        int dport = 0;
        if (ip->protocol == IPPROTO_UDP) {
            auto *udp = reinterpret_cast<udphdr *>(payload + ip->ihl * 4);
            dport = ntohs(udp->dest);
        }
        where = std::string(src) + " -> " + dst + ":" + std::to_string(dport);
    }
    std::println("  [{}] id={} len={} {}", g_seen, id, len, where);

    switch (g_mode) {
    case Mode::Drop:
        return nfq_set_verdict(qh, id, NF_DROP, 0, nullptr);

    case Mode::Delay:
        // Postpone: return without a verdict, settle it from the poll loop.
        g_held.push_back({id, Clock::now() + std::chrono::milliseconds(g_delayMs)});
        return 0;

    case Mode::Mangle:
        if (payload && manglePayload(payload, len)) {
            return nfq_set_verdict(qh, id, NF_ACCEPT,
                                   static_cast<std::uint32_t>(len), payload);
        }
        return nfq_set_verdict(qh, id, NF_ACCEPT, 0, nullptr);

    default:
        return nfq_set_verdict(qh, id, NF_ACCEPT, 0, nullptr);
    }
}

void releaseDuePackets() {
    const auto now = Clock::now();
    while (!g_held.empty() && g_held.front().due <= now) {
        nfq_set_verdict(g_qh, g_held.front().id, NF_ACCEPT, 0, nullptr);
        std::println("  released id={} after {}ms", g_held.front().id, g_delayMs);
        g_held.pop_front();
    }
}

} // namespace

int main(int argc, char **argv) {
    int queueNum  = 0;
    int runMs     = 4000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "accept") g_mode = Mode::Accept;
        else if (a == "drop")   g_mode = Mode::Drop;
        else if (a == "delay")  g_mode = Mode::Delay;
        else if (a == "mangle") g_mode = Mode::Mangle;
        else if (a == "--queue" && i + 1 < argc) queueNum = std::atoi(argv[++i]);
        else if (a == "--ms"    && i + 1 < argc) runMs    = std::atoi(argv[++i]);
        else if (a == "--delay" && i + 1 < argc) g_delayMs = std::atoi(argv[++i]);
    }

    nfq_handle *h = nfq_open();
    if (!h) { std::println("nfq_open failed (need root?)"); return 1; }

    // Legacy unbind/bind: harmless on modern kernels, required on old ones.
    nfq_unbind_pf(h, AF_INET);
    if (nfq_bind_pf(h, AF_INET) < 0) {
        std::println("nfq_bind_pf failed");
        nfq_close(h);
        return 1;
    }

    g_qh = nfq_create_queue(h, queueNum, &callback, nullptr);
    if (!g_qh) {
        std::println("nfq_create_queue({}) failed", queueNum);
        nfq_close(h);
        return 1;
    }
    if (nfq_set_mode(g_qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
        std::println("nfq_set_mode failed");
        nfq_destroy_queue(g_qh);
        nfq_close(h);
        return 1;
    }

    const int fd = nfq_fd(h);
    std::println("nfqtest: queue={} mode={} listening for {}ms",
                 queueNum, modeName(g_mode), runMs);
    std::fflush(stdout);

    char buf[65536] __attribute__((aligned));
    const auto deadline = Clock::now() + std::chrono::milliseconds(runMs);

    while (Clock::now() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        const int pr = poll(&pfd, 1, 50);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            const int rv = recv(fd, buf, sizeof(buf), 0);
            if (rv > 0) nfq_handle_packet(h, buf, rv);
        }
        releaseDuePackets();
    }

    // Flush anything still held so the test does not leave packets stranded.
    for (auto &held : g_held) nfq_set_verdict(g_qh, held.id, NF_ACCEPT, 0, nullptr);
    g_held.clear();

    std::println("nfqtest: done, {} packets seen", g_seen);
    nfq_destroy_queue(g_qh);
    nfq_close(h);
    return 0;
}
