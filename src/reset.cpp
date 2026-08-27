// Reset injection packet module
#include <stdlib.h>
#include <string.h>
#include "common.h"
#define NAME "reset"

static volatile short resetEnabled = 0,
    resetInbound = 1, resetOutbound = 1,
    chance = 0, // [0-10000]
    setNextCount = 0;


static void resetStartup() {
    LOG("reset enabled");
    InterlockedExchange16(&setNextCount, 0);
}

static void resetCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("reset disabled");
    InterlockedExchange16(&setNextCount, 0);
}

static short resetProcess(PacketNode *head, PacketNode *tail) {
    short reset = FALSE;
    // Phase 4.1: the header walking lives behind packetSetTcpRst() now, so this
    // module no longer knows which capture backend is underneath.
    const UINT tcpMinSize = packetMinTcpSize();
    PacketNode *pac = head->next;
    while (pac != tail) {
        if (checkDirection(pac->meta.outbound, resetInbound, resetOutbound)
            && pac->packetLen > tcpMinSize
            && (setNextCount || calcChance(chance)))
        {
            if (packetSetTcpRst(pac->packet, pac->packetLen)) {
                LOG("injecting reset w/ chance %.1f%%", chance/100.0);
                InterlockedIncrement(&resetModule.affectedCount);
                reset = TRUE;
                if (setNextCount > 0) {
                    InterlockedDecrement16(&setNextCount);
                }
            }
        }

        pac = pac->next;
    }
    return reset;
}

static int resetSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&chance, clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&resetInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&resetOutbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-next") == 0) {
        // write-only trigger: RST the next matching packet regardless of chance
        if (parseBoolValue(value) && resetEnabled) {
            InterlockedIncrement16(&setNextCount);
        }
        return 1;
    }
    return 0;
}

static int resetGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 3) return 0;
    strcpy(kv[n].key, NAME"-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, resetInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, resetOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec resetParamSpecs[] = {
    { NAME"-inbound",  "Inbound",          "bool",    0, 0 },
    { NAME"-outbound", "Outbound",         "bool",    0, 0 },
    { NAME"-chance",   "Chance (%)",       "percent", 0, 100 },
    { NAME"-next",     "RST next packet",  "action",  0, 0 },
};

Module resetModule = {
    "Set TCP RST",
    NAME,
    (short*)&resetEnabled,
    resetStartup,
    resetCloseDown,
    resetProcess,
    resetSetParam,
    resetGetParams,
    resetParamSpecs,
    (int)(sizeof(resetParamSpecs) / sizeof(resetParamSpecs[0])),
    // runtime fields
    0, 0, 0
};