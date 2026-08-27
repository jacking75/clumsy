// tampering packet module
//
// Tampers 1/4 of the packet payload using XOR patterns.
// Position of the tampered region is selectable:
//   Front  (0) — first quarter of the payload
//   Center (1) — middle quarter (original behavior)
//   Back   (2) — last quarter of the payload
//   Random (3) — random offset each time
//
// Short packets (<=4 bytes) always have the entire payload tampered.

#include <stdlib.h>
#include <string.h>
#include "common.h"

#define NAME "tamper"

#define POS_FRONT   0
#define POS_CENTER  1
#define POS_BACK    2
#define POS_RANDOM  3

static volatile short tamperEnabled = 0,
    tamperInbound  = 1,
    tamperOutbound = 1,
    chance         = 1000, // [0-10000]
    doChecksum     = 1,    // recompute checksum after tampering
    tamperPos      = POS_CENTER;

// patterns covers every bit combination
#define PATTERN_CNT 8
static const unsigned char patterns[] = {
    0x64,
    0x13,
    0x88,

    0x40,
    0x1F,
    0xA0,

    0xAA,
    0x55
};
static int patIx;

static void tamperStartup() {
    patIx = 0;
    LOG("tamper enabled, pos=%d", tamperPos);
}

static void tamperCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("tamper disabled");
}

static INLINE_FUNCTION void tamper_buf(char *buf, UINT len) {
    UINT ix;
    for (ix = 0; ix < len; ++ix) {
        buf[ix] = (char)(buf[ix] ^ patterns[patIx++ & 0x7]);
    }
}

static short tamperProcess(PacketNode *head, PacketNode *tail) {
    short tampered = FALSE;
    PacketNode *pac = head->next;
    while (pac != tail) {
        if (checkDirection(pac->meta.outbound, tamperInbound, tamperOutbound)
            && calcChance(chance)) {
            char *data  = NULL;
            UINT dataLen = 0;
            // Phase 4.1: payload lookup goes through the backend-neutral helper.
            if (packetGetPayload(pac->packet, pac->packetLen, &data, &dataLen)) {

                if (dataLen <= 4) {
                    // short packet: tamper everything
                    tamper_buf(data, dataLen);
                    LOG("tampered short packet (%u bytes)", dataLen);
                } else {
                    UINT len_d4 = dataLen / 4;
                    UINT offset;
                    short pos = tamperPos; // snapshot volatile

                    switch (pos) {
                    case POS_FRONT:
                        offset = 0;
                        break;
                    case POS_BACK:
                        offset = dataLen - len_d4;
                        break;
                    case POS_RANDOM:
                        offset = (UINT)(rand() % (int)(dataLen - len_d4 + 1));
                        break;
                    default: // POS_CENTER
                        offset = dataLen / 2 - len_d4 / 2 + 1;
                        break;
                    }

                    tamper_buf(data + offset, len_d4);
                    LOG("tampered pos=%d offset=%u len_d4=%u dataLen=%u",
                        pos, offset, len_d4, dataLen);
                }

                if (doChecksum) {
                    packetRecalcChecksums(pac->packet, pac->packetLen);
                }
                InterlockedIncrement(&tamperModule.affectedCount);
                tampered = TRUE;
            }
        }
        pac = pac->next;
    }
    return tampered;
}

static int tamperSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&chance, clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-position") == 0) {
        InterlockedExchange16(&tamperPos, clampShort(atoi(value), POS_FRONT, POS_RANDOM));
        return 1;
    }
    if (strcmp(key, NAME"-checksum") == 0) {
        InterlockedExchange16(&doChecksum, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&tamperInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&tamperOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int tamperGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 5) return 0;
    strcpy(kv[n].key, NAME"-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f); n++;
    strcpy(kv[n].key, NAME"-position");
    sprintf(kv[n].val, "%d", (int)tamperPos); n++;
    strcpy(kv[n].key, NAME"-checksum");
    strcpy(kv[n].val, doChecksum ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, tamperInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, tamperOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec tamperParamSpecs[] = {
    { NAME"-inbound",  "Inbound",                            "bool",    0, 0 },
    { NAME"-outbound", "Outbound",                           "bool",    0, 0 },
    { NAME"-checksum", "Redo checksum",                      "bool",    0, 0 },
    { NAME"-chance",   "Chance (%)",                         "percent", 0, 100 },
    { NAME"-position", "Position (0=Front 1=Center 2=Back 3=Random)", "int", 0, 3 },
};

Module tamperModule = {
    "Tamper",
    NAME,
    (short*)&tamperEnabled,
    tamperStartup,
    tamperCloseDown,
    tamperProcess,
    tamperSetParam,
    tamperGetParams,
    tamperParamSpecs,
    (int)(sizeof(tamperParamSpecs) / sizeof(tamperParamSpecs[0])),
    // runtime fields
    0, 0, 0
};
