// Packet inspection helpers, Linux implementation  (Phase 4.2)
//
// Same four-function contract as packetutil_win.cpp; tests/packetutil_test.cpp
// is the shared conformance test. WinDivert ships helper routines for this;
// on Linux we walk the headers ourselves, which is about a hundred lines
// because clumsy only ever needs IPv4/IPv6 with TCP/UDP/ICMP.

#include <string.h>

#include "common.h"

namespace {

constexpr unsigned IPV4_MIN_HDR = 20;
constexpr unsigned IPV6_HDR_LEN = 40;
constexpr unsigned TCP_MIN_HDR  = 20;
constexpr unsigned UDP_HDR_LEN  = 8;

constexpr unsigned char PROTO_TCP  = 6;
constexpr unsigned char PROTO_UDP  = 17;

constexpr unsigned TCP_FLAGS_OFFSET = 13;
constexpr unsigned char TCP_FLAG_RST = 0x04;

inline UINT16 read16be(const unsigned char *p) {
    return (UINT16)((p[0] << 8) | p[1]);
}
inline void write16be(unsigned char *p, UINT16 v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xFF);
}

// One's complement sum used by every IP checksum.
UINT32 sum16(const unsigned char *data, unsigned len, UINT32 acc = 0) {
    unsigned i = 0;
    for (; i + 1 < len; i += 2) acc += read16be(data + i);
    if (i < len) acc += (UINT32)data[i] << 8;   // odd trailing byte, high half
    return acc;
}

UINT16 foldChecksum(UINT32 acc) {
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (UINT16)(~acc & 0xFFFF);
}

// Describes where the transport header and payload live inside a packet.
struct Layout {
    bool     valid       = false;
    bool     isV4        = false;
    unsigned ipHdrLen    = 0;
    unsigned char proto  = 0;
    unsigned transportOffset = 0;
    unsigned transportLen    = 0;   // header only
    unsigned payloadOffset   = 0;
    unsigned payloadLen      = 0;
    bool     fragment    = false;   // non-first fragment: no transport header
};

Layout parse(const char *packet, UINT len) {
    Layout L;
    const unsigned char *p = (const unsigned char *)packet;
    if (!packet || len < IPV4_MIN_HDR) return L;

    const unsigned version = p[0] >> 4;
    unsigned ipTotal;

    if (version == 4) {
        L.isV4     = true;
        L.ipHdrLen = (p[0] & 0x0F) * 4;
        if (L.ipHdrLen < IPV4_MIN_HDR || len < L.ipHdrLen) return L;
        L.proto    = p[9];
        ipTotal    = read16be(p + 2);
        if (ipTotal > len || ipTotal < L.ipHdrLen) ipTotal = len;   // trust the buffer
        L.fragment = (read16be(p + 6) & 0x1FFF) != 0;
    } else if (version == 6) {
        if (len < IPV6_HDR_LEN) return L;
        L.isV4     = false;
        L.ipHdrLen = IPV6_HDR_LEN;
        // Extension headers are not walked: clumsy's modules only care about
        // plain TCP/UDP, and a packet carrying extension headers simply reports
        // no payload rather than being mis-parsed.
        L.proto    = p[6];
        ipTotal    = IPV6_HDR_LEN + read16be(p + 4);
        if (ipTotal > len) ipTotal = len;
    } else {
        return L;
    }

    L.transportOffset = L.ipHdrLen;
    if (L.fragment) { L.valid = true; return L; }

    if (L.proto == PROTO_TCP) {
        if (len < L.transportOffset + TCP_MIN_HDR) return L;
        L.transportLen = (unsigned)((p[L.transportOffset + 12] >> 4) * 4);
        if (L.transportLen < TCP_MIN_HDR ||
            len < L.transportOffset + L.transportLen) return L;
    } else if (L.proto == PROTO_UDP) {
        if (len < L.transportOffset + UDP_HDR_LEN) return L;
        L.transportLen = UDP_HDR_LEN;
    } else {
        L.valid = true;    // ICMP and friends: no ports, no payload handling
        return L;
    }

    L.payloadOffset = L.transportOffset + L.transportLen;
    L.payloadLen    = (ipTotal > L.payloadOffset) ? ipTotal - L.payloadOffset : 0;
    L.valid = true;
    return L;
}

// TCP and UDP checksums cover a pseudo-header built from the IP addresses.
UINT32 pseudoHeaderSum(const unsigned char *p, const Layout &L, unsigned transportTotal) {
    UINT32 acc = 0;
    if (L.isV4) {
        acc = sum16(p + 12, 8);                       // src + dst
        acc += (UINT32)L.proto;
        acc += (UINT32)transportTotal;
    } else {
        acc = sum16(p + 8, 32);                       // src + dst
        acc += (UINT32)(transportTotal >> 16) & 0xFFFF;
        acc += (UINT32)transportTotal & 0xFFFF;
        acc += (UINT32)L.proto;
    }
    return acc;
}

} // namespace

int packetGetPayload(char *packet, UINT len, char **payload, UINT *payloadLen) {
    if (!packet || len == 0 || !payload || !payloadLen) return 0;

    const Layout L = parse(packet, len);
    if (!L.valid || L.fragment || L.payloadLen == 0) return 0;
    if (L.payloadOffset + L.payloadLen > len) return 0;

    *payload    = packet + L.payloadOffset;
    *payloadLen = L.payloadLen;
    return 1;
}

void packetRecalcChecksums(char *packet, UINT len) {
    unsigned char *p = (unsigned char *)packet;
    if (!packet || len == 0) return;

    const Layout L = parse(packet, len);
    if (!L.valid) return;

    // --- IPv4 header checksum (IPv6 has none) ---
    if (L.isV4) {
        write16be(p + 10, 0);
        write16be(p + 10, foldChecksum(sum16(p, L.ipHdrLen)));
    }

    if (L.fragment) return;
    if (L.proto != PROTO_TCP && L.proto != PROTO_UDP) return;

    const unsigned transportTotal = L.transportLen + L.payloadLen;
    if (L.transportOffset + transportTotal > len) return;

    const unsigned cksumOffset = L.transportOffset + (L.proto == PROTO_TCP ? 16 : 6);
    if (cksumOffset + 2 > len) return;

    write16be(p + cksumOffset, 0);
    UINT32 acc = pseudoHeaderSum(p, L, transportTotal);
    acc = sum16(p + L.transportOffset, transportTotal, acc);
    UINT16 ck = foldChecksum(acc);
    // RFC 768: a zero UDP checksum means "not computed", so send 0xFFFF instead.
    if (L.proto == PROTO_UDP && ck == 0) ck = 0xFFFF;
    write16be(p + cksumOffset, ck);
}

int packetSetTcpRst(char *packet, UINT len) {
    unsigned char *p = (unsigned char *)packet;
    if (!packet || len == 0) return 0;

    const Layout L = parse(packet, len);
    if (!L.valid || L.fragment || L.proto != PROTO_TCP) return 0;
    if (L.transportOffset + TCP_MIN_HDR > len) return 0;

    p[L.transportOffset + TCP_FLAGS_OFFSET] |= TCP_FLAG_RST;
    packetRecalcChecksums(packet, len);
    return 1;
}

UINT packetMinTcpSize(void) {
    return (UINT)(IPV4_MIN_HDR + TCP_MIN_HDR);
}
