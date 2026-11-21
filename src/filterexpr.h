// Filter expression layer  (Phase 4.3)
//
// On Windows the filter string goes straight to WinDivertOpen and the kernel
// driver evaluates it. Linux has no equivalent: iptables decides what reaches
// the NFQUEUE, and that is a rule, not an expression.
//
// So on Linux the two roles are split:
//   * the operator's iptables rule selects which traffic enters the queue
//   * this evaluator re-applies clumsy's filter expression to decide which of
//     those packets clumsy actually degrades (everything else is accepted
//     untouched)
//
// Keeping the same expression syntax on both platforms means config.json
// presets, scenarios and automation scripts are portable unchanged.
//
// Supported subset of the WinDivert filter language:
//   booleans      and / or / not / && / || / !, parentheses, true / false
//   direction     inbound, outbound, loopback
//   protocols     ip, ipv6, tcp, udp, icmp, icmpv6
//   fields        ip.SrcAddr, ip.DstAddr            (dotted IPv4 literal)
//                 tcp.SrcPort, tcp.DstPort
//                 udp.SrcPort, udp.DstPort
//                 ip.Protocol                        (number)
//   comparisons   == != > < >= <=   (= is accepted as ==)
//
// Deliberately unsupported: ipv6 address literals, payload matching,
// packet/flow counters. Anything unrecognised is a compile error rather than a
// silent mismatch, so a filter that would behave differently from Windows is
// reported at Start instead of quietly degrading the wrong traffic.
#pragma once

#include "common.h"

struct FilterProgram;

// Parses expr. Returns NULL on error with errBuf filled.
// An empty or all-whitespace expression compiles to "match everything".
FilterProgram* filterCompile(const char *expr, char *errBuf, int errSize);

// Evaluates the compiled expression against one packet. packet points at the
// IP header. Returns 1 when clumsy should act on this packet.
int filterMatch(const FilterProgram *prog, const PacketMeta *meta,
                const char *packet, UINT len);

void filterFree(FilterProgram *prog);

// ---------------------------------------------------------------------------
// Overlap analysis  (multi-instance)
//
// Several clumsy instances may run at once, each shaping its own traffic. A
// packet that two of them match is shaped by whichever sees it first and
// silently skipped by the other, so a capture whose filter could match the
// same packet as a running instance's is refused.
//
// filterOverlap() decides whether any single packet could satisfy both
// expressions. It reads the full WinDivert syntax leniently: terms outside the
// subset above (tcp.Syn, ipv6.SrcAddr, impostor, ...) are kept as opaque
// conditions with no known relation to anything else. That can only make the
// answer "overlap" more often, never wrongly "disjoint".
//
// Returns FILTER_DISJOINT, FILTER_OVERLAP (msg describes an example packet both
// would match) or FILTER_UNKNOWN (msg says why it could not be decided).
// ---------------------------------------------------------------------------
#define FILTER_DISJOINT  0
#define FILTER_OVERLAP   1
#define FILTER_UNKNOWN  -1

int filterOverlap(const char *exprA, const char *exprB, char *msg, int msgSize);

// ---------------------------------------------------------------------------
// iptables rule derivation  (Phase 4.3 second tier, --auto-iptables)
//
// Turns the expression into iptables match arguments so clumsy can install the
// NFQUEUE rules itself instead of making the operator hand-write them.
//
// The derived rules are deliberately a SUPERSET of what the expression matches.
// Over-capturing is harmless - filterMatch() re-filters every packet and passes
// non-matching ones straight through - whereas under-capturing would silently
// miss the traffic under test. Anything that cannot be translated therefore
// widens the rule rather than narrowing it.
// ---------------------------------------------------------------------------
#define IPT_CHAIN_OUTPUT 1
#define IPT_CHAIN_INPUT  2

typedef struct {
    int  chains;                 // bit mask of IPT_CHAIN_*
    char match[256];             // iptables match args, e.g. "-p udp --dport 7777"
    int  ipv6;                   // 1 = install with ip6tables instead
    char expr[256];              // the same rule as a filter expression, for
                                 // filterOverlap(): what really enters the queue
} IptablesRule;

#define MAX_DERIVED_RULES 8

// Fills rules[] and returns how many were produced (>=1). exact is set to 0 when
// the rules are wider than the expression, which is worth telling the operator.
int filterDeriveIptables(const FilterProgram *prog, IptablesRule *rules,
                         int maxRules, int *exact);

// Writes the traffic the derived rules send to the queue as one expression
// (the rules' expr fields joined with "or"). With --auto-iptables that, not the
// filter, is what another instance's rules compete with. Returns 0 if the
// filter does not compile or the result does not fit.
int filterCaptureExpr(const char *filter, char *out, int outSize);
