// Packet inspection helpers, WinDivert implementation  (Phase 4.1)
//
// The modules used to call WinDivertHelperParsePacket / WinDivertHelperCalcChecksums
// directly, which tied tamper.cpp and reset.cpp to the Windows capture backend.
// They now go through the four functions declared in common.h, and this file is
// the only place outside divert.cpp that includes windivert.h.
//
// The Linux port adds packetutil_linux.cpp implementing the same four functions
// over plain IPv4/IPv6 header walking; no module changes.

#include <Windows.h>
#include "windivert.h"
#include "common.h"

int packetGetPayload(char *packet, UINT len, char **payload, UINT *payloadLen) {
    PVOID data = NULL;
    UINT dataLen = 0;

    if (!packet || len == 0 || !payload || !payloadLen) return 0;

    if (!WinDivertHelperParsePacket(packet, len,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            &data, &dataLen, NULL, NULL)) {
        return 0;
    }
    if (data == NULL || dataLen == 0) return 0;

    *payload    = (char*)data;
    *payloadLen = dataLen;
    return 1;
}

void packetRecalcChecksums(char *packet, UINT len) {
    if (!packet || len == 0) return;
    WinDivertHelperCalcChecksums(packet, len, NULL, 0);
}

int packetSetTcpRst(char *packet, UINT len) {
    PWINDIVERT_TCPHDR tcpHdr = NULL;

    if (!packet || len == 0) return 0;

    WinDivertHelperParsePacket(packet, len,
        NULL, NULL, NULL, NULL, NULL, &tcpHdr, NULL, NULL, NULL, NULL, NULL);

    if (tcpHdr == NULL) return 0;

    tcpHdr->Rst = 1;
    WinDivertHelperCalcChecksums(packet, len, NULL, 0);
    return 1;
}

UINT packetMinTcpSize(void) {
    return (UINT)(sizeof(WINDIVERT_IPHDR) + sizeof(WINDIVERT_TCPHDR));
}
