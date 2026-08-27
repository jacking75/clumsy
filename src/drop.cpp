// dropping packet module
#include <stdlib.h>
#include <string.h>
#include "common.h"
#define NAME "drop"

static volatile short dropEnabled = 0,
    dropInbound = 1, dropOutbound = 1,
    chance = 1000; // [0-10000]


static void dropStartUp() {
    LOG("drop enabled");
}

static void dropCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("drop disabled");
}

static short dropProcess(PacketNode *head, PacketNode* tail) {
    int dropped = 0;
    while (head->next != tail) {
        PacketNode *pac = head->next;
        // chance in range of [0, 10000]
        if (checkDirection(pac->meta.outbound, dropInbound, dropOutbound)
            && calcChance(chance)) {
            LOG("dropped with chance %.1f%%, direction %s",
                chance/100.0, pac->meta.outbound ? "OUTBOUND" : "INBOUND");
            freeNode(popNode(pac));
            ++dropped;
            InterlockedIncrement(&dropModule.affectedCount);
        } else {
            head = head->next;
        }
    }

    return dropped > 0;
}


static int dropSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&chance, clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&dropInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&dropOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int dropGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 3) return 0;
    strcpy(kv[n].key, NAME"-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, dropInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, dropOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec dropParamSpecs[] = {
    { NAME"-inbound",  "Inbound",    "bool",    0, 0 },
    { NAME"-outbound", "Outbound",   "bool",    0, 0 },
    { NAME"-chance",   "Chance (%)", "percent", 0, 100 },
};

Module dropModule = {
    "Drop",
    NAME,
    (short*)&dropEnabled,
    dropStartUp,
    dropCloseDown,
    dropProcess,
    dropSetParam,
    dropGetParams,
    dropParamSpecs,
    (int)(sizeof(dropParamSpecs) / sizeof(dropParamSpecs[0])),
    // runtime fields
    0, 0, 0
};
