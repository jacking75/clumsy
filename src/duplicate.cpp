// duplicate packet module
#include <stdlib.h>
#include <string.h>
#include "common.h"
#define NAME "duplicate"
#define COPIES_MIN "2"
#define COPIES_MAX "50"
#define COPIES_COUNT 2

static volatile short dupEnabled = 0,
    dupInbound = 1, dupOutbound = 1,
    chance = 1000, // [0-10000]
    count = COPIES_COUNT; // how many copies to duplicate

static void dupStartup() {
    LOG("dup enabled");
}

static void dupCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("dup disabled");
}

static short dupProcess(PacketNode *head, PacketNode *tail) {
    short duped = FALSE;
    PacketNode *pac = head->next;
    while (pac != tail) {
        if (checkDirection(pac->meta.outbound, dupInbound, dupOutbound)
            && calcChance(chance)) {
            short copies = count - 1;
            LOG("duplicating w/ chance %.1f%%, cloned additionally %d packets", chance/100.0, copies);
            while (copies--) {
                PacketNode *copy = cloneNode(pac);
                insertBefore(copy, pac); // must insertBefore or next packet is still pac
                InterlockedIncrement(&dupModule.affectedCount);
            }
            duped = TRUE;
        }
        pac = pac->next;
    }
    return duped;
}

static int dupSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&chance, clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-count") == 0) {
        InterlockedExchange16(&count, clampShort(atoi(value), 2, 50));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&dupInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&dupOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int dupGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 4) return 0;
    strcpy(kv[n].key, NAME"-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f); n++;
    strcpy(kv[n].key, NAME"-count");
    sprintf(kv[n].val, "%d", (int)count); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, dupInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, dupOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec dupParamSpecs[] = {
    { NAME"-inbound",  "Inbound",    "bool",    0, 0 },
    { NAME"-outbound", "Outbound",   "bool",    0, 0 },
    { NAME"-count",    "Copies",     "int",     2, 50 },
    { NAME"-chance",   "Chance (%)", "percent", 0, 100 },
};

Module dupModule = {
    "Duplicate",
    NAME,
    (short*)&dupEnabled,
    dupStartup,
    dupCloseDown,
    dupProcess,
    dupSetParam,
    dupGetParams,
    dupParamSpecs,
    (int)(sizeof(dupParamSpecs) / sizeof(dupParamSpecs[0])),
    // runtime fields
    0, 0, 0
};