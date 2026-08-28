// throttling packets
#include <stdlib.h>
#include <string.h>
#include "common.h"
#define NAME "throttle"
#define TIME_MIN "0"
#define TIME_MAX "1000"
#define TIME_DEFAULT 30
// threshold for how many packet to throttle at most
#define KEEP_AT_MOST 1000

static volatile short throttleEnabled = 0,
    throttleInbound = 1, throttleOutbound = 1,
    chance = 1000, // [0-10000]
    // time frame in ms, when a throttle start the packets within the time 
    // will be queued and sent altogether when time is over
    throttleFrame = TIME_DEFAULT,
    dropThrottled = 0; 

static PacketNode throttleHeadNode = {0}, throttleTailNode = {0};
static PacketNode *bufHead = &throttleHeadNode, *bufTail = &throttleTailNode;
static int bufSize = 0;
static DWORD throttleStartTick = 0;

static INLINE_FUNCTION short isBufEmpty() {
    short ret = bufHead->next == bufTail;
    if (ret) assert(bufSize == 0);
    return ret;
}

static void throttleStartUp() {
    if (bufHead->next == NULL && bufTail->next == NULL) {
        bufHead->next = bufTail;
        bufTail->prev = bufHead;
        bufSize = 0;
    } else {
        assert(isBufEmpty());
    }
    throttleStartTick = 0;
    startTimePeriod();
}

static void clearBufPackets(PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    LOG("Throttled end, send all %d packets. Buffer at max: %s", bufSize, bufSize == KEEP_AT_MOST ? "YES" : "NO");
    while (!isBufEmpty()) {
        insertAfter(popNode(bufTail->prev), oldLast);
        --bufSize;
    }
    throttleStartTick = 0;
}

static void dropBufPackets() {
    LOG("Throttled end, drop all %d packets. Buffer at max: %s", bufSize, bufSize == KEEP_AT_MOST ? "YES" : "NO");
    while (!isBufEmpty()) {
        freeNode(popNode(bufTail->prev));
        --bufSize;
    }
    throttleStartTick = 0;
}


static void throttleCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(tail);
    UNREFERENCED_PARAMETER(head);
    clearBufPackets(tail);
    endTimePeriod();
}

static short throttleProcess(PacketNode *head, PacketNode *tail) {
    short throttled = FALSE;
    UNREFERENCED_PARAMETER(head);
    if (!throttleStartTick) {
        if (!isListEmpty() && calcChance(chance)) {
            // chance is in hundredths of a percent, like every other module's:
            // /100.0 to print it as a percentage. It used to say /10.0, which
            // reported the 10% default as "100.0".
            LOG("Start new throttling w/ chance %.1f%%, time frame: %d",
                chance/100.0, throttleFrame);
            throttleStartTick = timeGetTime();
            throttled = TRUE;
            goto THROTTLE_START; // need this goto since maybe we'll start and stop at this single call
        }
    } else {
THROTTLE_START:
        // start a block for declaring local variables
        {
            // already throttling, keep filling up
            PacketNode *pac = tail->prev;
            DWORD currentTick = timeGetTime();
            while (bufSize < KEEP_AT_MOST && pac != head) {
                if (checkDirection(pac->meta.outbound, throttleInbound, throttleOutbound)) {
                    insertAfter(popNode(pac), bufHead);
                    ++bufSize;
                    InterlockedIncrement(&throttleModule.affectedCount);
                    pac = tail->prev;
                } else {
                    pac = pac->prev;
                }
            }

            // send all when throttled enough, including in current step
            if (bufSize >= KEEP_AT_MOST || (currentTick - throttleStartTick > (unsigned int)throttleFrame)) {
                // drop throttled if dropThrottled is toggled
                if (dropThrottled) {
                    dropBufPackets();
                } else {
                    clearBufPackets(tail);
                }
            }
        }
    }

    return throttled;
}

static int throttleSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&chance, clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-frame") == 0) {
        InterlockedExchange16(&throttleFrame, clampShort(atoi(value), 0, 1000));
        return 1;
    }
    if (strcmp(key, NAME"-drop") == 0) {
        InterlockedExchange16(&dropThrottled, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&throttleInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&throttleOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int throttleGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 5) return 0;
    strcpy(kv[n].key, NAME"-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f); n++;
    strcpy(kv[n].key, NAME"-frame");
    sprintf(kv[n].val, "%d", (int)throttleFrame); n++;
    strcpy(kv[n].key, NAME"-drop");
    strcpy(kv[n].val, dropThrottled ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, throttleInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, throttleOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec throttleParamSpecs[] = {
    { NAME"-inbound",  "Inbound",         "bool",    0, 0 },
    { NAME"-outbound", "Outbound",        "bool",    0, 0 },
    { NAME"-drop",     "Drop throttled",  "bool",    0, 0 },
    { NAME"-frame",    "Timeframe (ms)",  "int",     0, 1000 },
    { NAME"-chance",   "Chance (%)",      "percent", 0, 100 },
};

Module throttleModule = {
    "Throttle",
    NAME,
    (short*)&throttleEnabled,
    throttleStartUp,
    throttleCloseDown,
    throttleProcess,
    throttleSetParam,
    throttleGetParams,
    throttleParamSpecs,
    (int)(sizeof(throttleParamSpecs) / sizeof(throttleParamSpecs[0])),
    // runtime fields
    0, 0, 0
};