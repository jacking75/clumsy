// jitter module - random delay per packet within [min, max] range
#include <stdlib.h>
#include <string.h>
#include "common.h"

#define NAME "jitter"
#define JITTER_MIN_DEFAULT 20
#define JITTER_MAX_DEFAULT 100
#define JITTER_TIME_MIN "0"
#define JITTER_TIME_MAX "5000"
#define KEEP_AT_MOST 2000
#define FLUSH_WHEN_FULL 800

static volatile short jitterEnabled = 0,
    jitterInbound = 1,
    jitterOutbound = 1,
    jitterMin = JITTER_MIN_DEFAULT,
    jitterMax = JITTER_MAX_DEFAULT;

static PacketNode jitterHeadNode = {0}, jitterTailNode = {0};
static PacketNode *bufHead = &jitterHeadNode, *bufTail = &jitterTailNode;
static int bufSize = 0;

static INLINE_FUNCTION short isBufEmpty() {
    short ret = bufHead->next == bufTail;
    if (ret) assert(bufSize == 0);
    return ret;
}

static void jitterStartUp() {
    if (bufHead->next == NULL && bufTail->next == NULL) {
        bufHead->next = bufTail;
        bufTail->prev = bufHead;
        bufSize = 0;
    } else {
        assert(isBufEmpty());
    }
    startTimePeriod();
}

static void jitterCloseDown(PacketNode *head, PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    UNREFERENCED_PARAMETER(head);
    LOG("Closing down jitter, flushing %d packets", bufSize);
    while (!isBufEmpty()) {
        insertAfter(popNode(bufTail->prev), oldLast);
        --bufSize;
    }
    endTimePeriod();
}

static short jitterProcess(PacketNode *head, PacketNode *tail) {
    DWORD currentTime = timeGetTime();
    PacketNode *pac = tail->prev;
    // snapshot volatile values once per call
    short lo = jitterMin, hi = jitterMax, range;

    // normalize in case user typed min > max
    if (lo > hi) { short tmp = lo; lo = hi; hi = tmp; }
    range = hi - lo;

    // move incoming packets into jitter buffer, assign each a random target send time
    while (bufSize < KEEP_AT_MOST && pac != head) {
        if (checkDirection(pac->meta.outbound, jitterInbound, jitterOutbound)) {
            DWORD delay = (DWORD)lo + (range > 0 ? (DWORD)(rand() % ((int)range + 1)) : 0);
            insertAfter(popNode(pac), bufHead)->timestamp = timeGetTime() + delay;
            ++bufSize;
            InterlockedIncrement(&jitterModule.affectedCount);
            pac = tail->prev;
        } else {
            pac = pac->prev;
        }
    }

    // release any packet whose scheduled time has arrived
    // scan the entire buffer because random delays can make later packets ready earlier
    pac = bufTail->prev;
    while (pac != bufHead) {
        PacketNode *prevPac = pac->prev;
        if (currentTime >= pac->timestamp) {
            LOG("jitter released packet, delay was %lums", (unsigned long)(currentTime - (pac->timestamp - (currentTime - pac->timestamp))));
            insertAfter(popNode(pac), head);
            --bufSize;
        }
        pac = prevPac;
    }

    // buffer overflow guard: flush oldest packets when full
    if (bufSize >= KEEP_AT_MOST) {
        int flushCnt = FLUSH_WHEN_FULL;
        while (flushCnt-- > 0 && !isBufEmpty()) {
            insertAfter(popNode(bufTail->prev), head);
            --bufSize;
        }
    }

    return bufSize > 0;
}

static int jitterSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-min") == 0) {
        InterlockedExchange16(&jitterMin, clampShort(atoi(value), 0, 5000));
        return 1;
    }
    if (strcmp(key, NAME"-max") == 0) {
        InterlockedExchange16(&jitterMax, clampShort(atoi(value), 0, 5000));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&jitterInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&jitterOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int jitterGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 4) return 0;
    strcpy(kv[n].key, NAME"-min");
    sprintf(kv[n].val, "%d", (int)jitterMin); n++;
    strcpy(kv[n].key, NAME"-max");
    sprintf(kv[n].val, "%d", (int)jitterMax); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, jitterInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, jitterOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec jitterParamSpecs[] = {
    { NAME"-inbound",  "Inbound",  "bool", 0, 0 },
    { NAME"-outbound", "Outbound", "bool", 0, 0 },
    { NAME"-min",      "Min (ms)", "int",  0, 5000 },
    { NAME"-max",      "Max (ms)", "int",  0, 5000 },
};

int jitterGetBufSize(void) { return bufSize; }

Module jitterModule = {
    "Jitter",
    NAME,
    (short*)&jitterEnabled,
    jitterStartUp,
    jitterCloseDown,
    jitterProcess,
    jitterSetParam,
    jitterGetParams,
    jitterParamSpecs,
    (int)(sizeof(jitterParamSpecs) / sizeof(jitterParamSpecs[0])),
    // runtime fields
    0, 0, 0
};
