// Cross-platform regression test for the Phase 4.1 packet helpers.
//
// The four functions declared in common.h are the entire surface tamper.cpp and
// reset.cpp use to inspect a packet. Phase 4.2 adds a second implementation
// (packetutil_linux.cpp) alongside packetutil_win.cpp; this test is the contract
// both must satisfy, so run it against each backend.
//
// Packets are assembled byte by byte from raw offsets rather than through any
// platform header, so the same source compiles on Windows and Linux.
//
// Build (Windows, from the repo root, in a VS developer prompt):
//   cl /nologo /std:c++latest /EHsc /DX64 /Isrc /Iexternal\WinDivert-2.2.0-A\include ^
//      tests\packetutil_test.cpp src\packetutil_win.cpp ^
//      /link /LIBPATH:external\WinDivert-2.2.0-A\x64 WinDivert.lib ws2_32.lib
//
// Build (Linux, Phase 4.2):
//   g++-16 -std=c++23 -Isrc tests/packetutil_test.cpp src/packetutil_linux.cpp -o packetutil_test

#include <cstdio>
#include <cstring>

#include "common.h"

namespace {

int failures = 0;

void check(const char *name, bool ok) {
    std::printf("%-36s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

// ---------------------------------------------------------------------------
// Raw packet assembly. Offsets follow RFC 791 / 793 / 768 so no platform
// headers are involved.
// ---------------------------------------------------------------------------

constexpr unsigned IP_HDR_LEN  = 20;
constexpr unsigned TCP_HDR_LEN = 20;
constexpr unsigned UDP_HDR_LEN = 8;

constexpr unsigned char PROTO_TCP = 6;
constexpr unsigned char PROTO_UDP = 17;

constexpr unsigned TCP_FLAGS_OFFSET = 13;   // within the TCP header
constexpr unsigned char TCP_FLAG_RST = 0x04;

void put16be(unsigned char *p, unsigned v) {
    p[0] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[1] = static_cast<unsigned char>(v & 0xFF);
}

void put32be(unsigned char *p, unsigned long v) {
    p[0] = static_cast<unsigned char>((v >> 24) & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[3] = static_cast<unsigned char>(v & 0xFF);
}

unsigned read16be(const unsigned char *p) {
    return (static_cast<unsigned>(p[0]) << 8) | p[1];
}

// Returns the total packet length.
unsigned buildPacket(char *out, unsigned char proto,
                     const char *payload, unsigned payloadLen) {
    auto *b = reinterpret_cast<unsigned char *>(out);
    const unsigned transportLen = (proto == PROTO_TCP) ? TCP_HDR_LEN : UDP_HDR_LEN;
    const unsigned total = IP_HDR_LEN + transportLen + payloadLen;

    std::memset(b, 0, total);

    // --- IPv4 header ---
    b[0] = 0x45;                                   // version 4, IHL 5 words
    put16be(b + 2, total);                         // total length
    b[8]  = 64;                                    // TTL
    b[9]  = proto;
    put32be(b + 12, 0x7F000001UL);                 // 127.0.0.1
    put32be(b + 16, 0x7F000001UL);

    // --- transport header ---
    unsigned char *t = b + IP_HDR_LEN;
    put16be(t + 0, 12345);                         // source port
    put16be(t + 2, (proto == PROTO_TCP) ? 80 : 9999);
    if (proto == PROTO_TCP) {
        t[12] = static_cast<unsigned char>((TCP_HDR_LEN / 4) << 4);  // data offset
        t[TCP_FLAGS_OFFSET] = 0x18;                // PSH|ACK
        put16be(t + 14, 8192);                     // window
    } else {
        put16be(t + 4, transportLen + payloadLen); // UDP length
    }

    if (payloadLen) {
        std::memcpy(b + IP_HDR_LEN + transportLen, payload, payloadLen);
    }

    // Let the implementation under test produce the initial checksums, which
    // also proves it can checksum a packet it did not itself parse before.
    packetRecalcChecksums(out, total);
    return total;
}

const unsigned char* tcpHeader(const char *pkt) {
    return reinterpret_cast<const unsigned char *>(pkt) + IP_HDR_LEN;
}

const unsigned char* udpHeader(const char *pkt) {
    return reinterpret_cast<const unsigned char *>(pkt) + IP_HDR_LEN;
}

} // namespace

int main() {
    char buf[512];
    char *payload = nullptr;
    UINT payloadLen = 0;
    unsigned len;

    std::printf("Phase 4.1 packet helper contract\n");
    std::printf("--------------------------------------------------------\n");

    check("packetMinTcpSize == 40", packetMinTcpSize() == IP_HDR_LEN + TCP_HDR_LEN);

    // --- tamper.cpp path: locate and rewrite a UDP payload ---
    len = buildPacket(buf, PROTO_UDP, "HELLO-WORLD", 11);
    check("UDP payload found",
          packetGetPayload(buf, len, &payload, &payloadLen) == 1);
    check("UDP payload length == 11", payloadLen == 11);
    check("UDP payload contents",
          payload != nullptr && std::memcmp(payload, "HELLO-WORLD", 11) == 0);
    check("payload points into the packet",
          payload == buf + IP_HDR_LEN + UDP_HDR_LEN);

    if (payload) {
        const unsigned before = read16be(udpHeader(buf) + 6);
        std::memset(payload, 'X', payloadLen);
        packetRecalcChecksums(buf, len);
        check("payload write lands in the packet",
              std::memcmp(buf + IP_HDR_LEN + UDP_HDR_LEN, "XXXXXXXXXXX", 11) == 0);
        check("recalcChecksums updates UDP cksum",
              read16be(udpHeader(buf) + 6) != before);
    }

    // --- a packet with no payload must report none ---
    len = buildPacket(buf, PROTO_TCP, nullptr, 0);
    payload = nullptr;
    payloadLen = 0;
    check("no payload -> returns 0",
          packetGetPayload(buf, len, &payload, &payloadLen) == 0);

    // --- reset.cpp path: set RST and re-checksum ---
    len = buildPacket(buf, PROTO_TCP, "REQ", 3);
    {
        const unsigned beforeCk = read16be(tcpHeader(buf) + 16);
        check("RST clear before the call",
              (tcpHeader(buf)[TCP_FLAGS_OFFSET] & TCP_FLAG_RST) == 0);
        check("packetSetTcpRst returns 1", packetSetTcpRst(buf, len) == 1);
        check("RST flag now set",
              (tcpHeader(buf)[TCP_FLAGS_OFFSET] & TCP_FLAG_RST) != 0);
        check("other TCP flags preserved",
              (tcpHeader(buf)[TCP_FLAGS_OFFSET] & 0x18) == 0x18);
        check("TCP checksum recomputed",
              read16be(tcpHeader(buf) + 16) != beforeCk);
    }

    // --- and it must refuse anything that is not TCP ---
    len = buildPacket(buf, PROTO_UDP, "DATA", 4);
    check("packetSetTcpRst on UDP -> 0", packetSetTcpRst(buf, len) == 0);

    // --- defensive inputs ---
    check("null packet handled",
          packetGetPayload(nullptr, 0, &payload, &payloadLen) == 0);
    check("zero length handled", packetSetTcpRst(buf, 0) == 0);

    std::printf("--------------------------------------------------------\n");
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
