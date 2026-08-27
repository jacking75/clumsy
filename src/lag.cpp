// lagging packets
#include <stdlib.h>
#include <string.h>
#include "common.h"
#define NAME "lag"
#define LAG_MIN "0"
#define LAG_MAX "15000"
#define KEEP_AT_MOST 2000
// send FLUSH_WHEN_FULL packets when buffer is full
#define FLUSH_WHEN_FULL 800
#define LAG_DEFAULT 50

// don't need a chance
static volatile short lagEnabled = 0,
    lagInbound = 1,
    lagOutbound = 1,
    lagTime = LAG_DEFAULT; // default for 50ms

static PacketNode lagHeadNode = {0}, lagTailNode = {0};
static PacketNode *bufHead = &lagHeadNode, *bufTail = &lagTailNode;
static int bufSize = 0;

static INLINE_FUNCTION short isBufEmpty() {
    short ret = bufHead->next == bufTail;
    if (ret) assert(bufSize == 0);
    return ret;
}

static void lagStartUp() {
    if (bufHead->next == NULL && bufTail->next == NULL) {
        bufHead->next = bufTail;
        bufTail->prev = bufHead;
        bufSize = 0;
    } else {
        assert(isBufEmpty());
    }
    startTimePeriod();
}

static void lagCloseDown(PacketNode *head, PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    UNREFERENCED_PARAMETER(head);
    // flush all buffered packets
    LOG("Closing down lag, flushing %d packets", bufSize);
    while(!isBufEmpty()) {
        insertAfter(popNode(bufTail->prev), oldLast);
        --bufSize;
    }
    endTimePeriod();
}

static short lagProcess(PacketNode *head, PacketNode *tail) {
    DWORD currentTime = timeGetTime();
    PacketNode *pac = tail->prev;
    // pick up all packets and fill in the current time
    while (bufSize < KEEP_AT_MOST && pac != head) {
        if (checkDirection(pac->meta.outbound, lagInbound, lagOutbound)) {
            insertAfter(popNode(pac), bufHead)->timestamp = timeGetTime();
            ++bufSize;
            InterlockedIncrement(&lagModule.affectedCount);
            pac = tail->prev;
        } else {
            pac = pac->prev;
        }
    }

    // try sending overdue packets from buffer tail
    while (!isBufEmpty()) {
        pac = bufTail->prev;
        if (currentTime > pac->timestamp + lagTime) {
            insertAfter(popNode(bufTail->prev), head); // sending queue is already empty by now
            --bufSize;
            LOG("Send lagged packets.");
        } else {
            LOG("Sent some lagged packets, still have %d in buf", bufSize);
            break;
        }
    }

    // if buffer is full just flush things out
    if (bufSize >= KEEP_AT_MOST) {
        int flushCnt = FLUSH_WHEN_FULL;
        while (flushCnt-- > 0) {
            insertAfter(popNode(bufTail->prev), head);
            --bufSize;
        }
    }

    return bufSize > 0;
}

static int lagSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-time") == 0) {
        InterlockedExchange16(&lagTime, clampShort(atoi(value), 0, 15000));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&lagInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&lagOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int lagGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 3) return 0;
    strcpy(kv[n].key, NAME"-time");
    sprintf(kv[n].val, "%d", (int)lagTime); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, lagInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, lagOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec lagParamSpecs[] = {
    { NAME"-inbound",  "Inbound",    "bool", 0, 0 },
    { NAME"-outbound", "Outbound",   "bool", 0, 0 },
    { NAME"-time",     "Delay (ms)", "int",  0, 15000 },
};

int lagGetBufSize(void) { return bufSize; }

Module lagModule = {
    "Lag",
    NAME,
    (short*)&lagEnabled,
    lagStartUp,
    lagCloseDown,
    lagProcess,
    lagSetParam,
    lagGetParams,
    lagParamSpecs,
    (int)(sizeof(lagParamSpecs) / sizeof(lagParamSpecs[0])),
    // runtime fields
    0, 0, 0
};