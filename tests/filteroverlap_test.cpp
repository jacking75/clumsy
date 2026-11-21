// Unit test for filterOverlap()  (multi-instance)
//
// Two clumsy instances whose filters can match the same packet would fight
// over it: one shapes it, the other silently never sees it. filterOverlap() is
// what refuses the second capture, so both of its failure modes are real bugs:
// a wrong "disjoint" lets the fight happen, a wrong "overlap" blocks the
// one-instance-per-client setup the feature exists for.
//
// Built and run by `make test`.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "filterexpr.h"

// filterexpr.cpp is the only thing under test; these satisfy the linker without
// pulling in the rest of clumsy.
volatile short logVerbose = 0;
void logPrintf(const char *fmt, ...) { (void)fmt; }
int appendf(char *buf, int bufSize, int pos, const char *fmt, ...) {
    va_list ap;
    if (pos < 0 || pos >= bufSize) return pos;
    va_start(ap, fmt);
    const int n = vsnprintf(buf + pos, (size_t)(bufSize - pos), fmt, ap);
    va_end(ap);
    if (n < 0) return pos;
    return (pos + n < bufSize) ? pos + n : bufSize - 1;
}

static int failures = 0;

static void expect(const char *what, const char *a, const char *b, int want) {
    char msg[512];
    const int got = filterOverlap(a, b, msg, sizeof(msg));
    const char *names[] = { "unknown", "disjoint", "overlap" };
    printf("%-58s %s", what, got == want ? "PASS" : "FAIL");
    if (got != want) {
        printf("   <- got %s, wanted %s (%s)", names[got + 1], names[want + 1], msg);
        failures++;
    }
    printf("\n");
}

// One client's traffic to and from the server, the filter the guide teaches.
// A top-level OR of AND-only branches, so --auto-iptables can derive exact rules.
static std::string clientFilter(int clientPort, int serverPort) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "(udp.SrcPort == %d and udp.DstPort == %d) or "
             "(udp.SrcPort == %d and udp.DstPort == %d)",
             clientPort, serverPort, serverPort, clientPort);
    return buf;
}

// What --process appends on Windows for a process with these ports.
static std::string processFragment(int firstPort, int count) {
    std::string s = " and (";
    for (int i = 0; i < count; ++i) {
        const int p = firstPort + i;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "%stcp.SrcPort == %d or tcp.DstPort == %d or udp.SrcPort == %d or udp.DstPort == %d",
                 i ? " or " : "", p, p, p, p);
        s += buf;
    }
    return s + ")";
}

int main(void) {
    printf("clumsy filter overlap\n");
    printf("--------------------------------------------------------------------------\n");

    // --- the setup the feature exists for: one instance per client ---
    expect("two clients of the same server are disjoint",
           clientFilter(50001, 7777).c_str(), clientFilter(50002, 7777).c_str(), FILTER_DISJOINT);
    expect("the same client twice overlaps",
           clientFilter(50001, 7777).c_str(), clientFilter(50001, 7777).c_str(), FILTER_OVERLAP);
    // Without the server port, a packet from 50001 to 50002 matches both.
    expect("client ports alone are not disjoint",
           "udp.SrcPort == 50001 or udp.DstPort == 50001",
           "udp.SrcPort == 50002 or udp.DstPort == 50002", FILTER_OVERLAP);
    expect("all clients vs one client overlaps",
           "udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)",
           clientFilter(50001, 7777).c_str(), FILTER_OVERLAP);
    expect("port ranges that do not meet are disjoint",
           "udp.SrcPort >= 50001 and udp.SrcPort <= 50010", "udp.SrcPort == 50011", FILTER_DISJOINT);
    expect("port ranges that meet overlap",
           "udp.SrcPort >= 50001 and udp.SrcPort <= 50010", "udp.SrcPort == 50010", FILTER_OVERLAP);
    expect("one remote host per direction pair is disjoint",
           "(ip.SrcAddr == 10.0.0.5 and ip.DstAddr == 10.0.0.9) or (ip.SrcAddr == 10.0.0.9 and ip.DstAddr == 10.0.0.5)",
           "(ip.SrcAddr == 10.0.0.6 and ip.DstAddr == 10.0.0.9) or (ip.SrcAddr == 10.0.0.9 and ip.DstAddr == 10.0.0.6)",
           FILTER_DISJOINT);
    expect("inbound vs outbound is disjoint", "inbound and udp", "outbound and udp", FILTER_DISJOINT);
    expect("IPv4 vs IPv6 is disjoint", "ip and tcp", "ipv6 and tcp", FILTER_DISJOINT);

    // --- negation follows evalNode(): a port test is false on a packet without that port ---
    expect("excluding the API port leaves it to another filter",
           "outbound and loopback and not (tcp.DstPort == 8080 or tcp.SrcPort == 8080)",
           "tcp.DstPort == 8080", FILTER_DISJOINT);
    expect("a bare != port test never matches another protocol",
           "tcp.DstPort != 8080", "udp", FILTER_DISJOINT);
    expect("a negated port test does match another protocol",
           "not (tcp.DstPort == 8080)", "udp", FILTER_OVERLAP);

    // --- WinDivert terms outside the subset ---
    expect("an unknown term and its negation are disjoint",
           "tcp.Syn and tcp.DstPort == 80", "not tcp.Syn and tcp.DstPort == 80", FILTER_DISJOINT);
    expect("two unrelated unknown terms overlap", "tcp.Syn", "tcp.Ack", FILTER_OVERLAP);
    expect("IPv6 and hex values parse",
           "ipv6.DstAddr == ::1 and udp.DstPort == 0x1E61", "udp.DstPort == 7777", FILTER_OVERLAP);
    expect("an unparsable filter is reported, not guessed",
           "udp and (", "udp", FILTER_UNKNOWN);

    // --- the overlap message has to tell the caller what to fix ---
    {
        char msg[512];
        filterOverlap(clientFilter(50001, 7777).c_str(), "udp.DstPort == 7777", msg, sizeof(msg));
        const int ok = strstr(msg, "50001") && strstr(msg, "7777");
        printf("%-58s %s\n", "overlap message names the shared ports", ok ? "PASS" : "FAIL");
        if (!ok) { printf("   <- %s\n", msg); failures++; }
    }

    // --- two --process filters of 64 ports each must still be decided ---
    {
        const std::string a = "(udp and (udp.SrcPort == 7777 or udp.DstPort == 7777))" + processFragment(40000, 64);
        const std::string b = "(udp and (udp.SrcPort == 7777 or udp.DstPort == 7777))" + processFragment(41000, 64);
        expect("two 64-port process filters are decided", a.c_str(), b.c_str(), FILTER_DISJOINT);
    }

    // --- Linux --auto-iptables: the queue rules, not the filters, compete ---
    // "loopback" cannot be expressed in iptables, so both rules send all
    // UDP-to-7777 to their queue; the first queue then takes the other's packets.
    {
        const char *fa = "outbound and loopback and udp.DstPort == 7777";
        const char *fb = "outbound and not loopback and udp.DstPort == 7777";
        char ca[1024], cb[1024];
        expect("filters split by loopback are disjoint", fa, fb, FILTER_DISJOINT);
        const int built = filterCaptureExpr(fa, ca, sizeof(ca)) && filterCaptureExpr(fb, cb, sizeof(cb));
        printf("%-58s %s\n", "capture expressions are derived", built ? "PASS" : "FAIL");
        if (!built) failures++;
        else expect("but their iptables rules overlap", ca, cb, FILTER_OVERLAP);

        char c1[1024], c2[1024];
        if (filterCaptureExpr(clientFilter(50001, 7777).c_str(), c1, sizeof(c1)) &&
            filterCaptureExpr(clientFilter(50002, 7777).c_str(), c2, sizeof(c2))) {
            expect("per-client rules stay disjoint", c1, c2, FILTER_DISJOINT);
        } else {
            printf("%-58s FAIL\n", "per-client capture expressions are derived");
            failures++;
        }
    }

    printf("--------------------------------------------------------------------------\n");
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
