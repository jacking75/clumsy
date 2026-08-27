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
