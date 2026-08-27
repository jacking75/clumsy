// Out of Order module - multi-packet buffer with random shuffle release
//
// Captures up to Buf packets from the stream. When the buffer is full OR the
// oldest held packet exceeds Delay(ms), all held packets are released in a
// randomly shuffled order, creating realistic out-of-order delivery.
//
// Previous design: held 1 packet, gave up after KEEP_TURNS_MAX steps,
//                  swapped adjacent pairs.
// New design:      holds up to 50 packets, time-based hold, Fisher-Yates
//                  shuffle on release.

#include <stdlib.h>
#include <string.h>
#include "common.h"

#define NAME "ood"
#define OOD_BUF_DEFAULT    5
#define OOD_DELAY_DEFAULT  200
#define OOD_BUF_MIN   "2"
#define OOD_BUF_MAX   "50"
#define OOD_DELAY_MIN "10"
#define OOD_DELAY_MAX "2000"

static volatile short oodEnabled   = 0,
    oodInbound   = 1,
    oodOutbound  = 1,
    chance       = 1000,              // [0-10000]
    maxBuffer    = OOD_BUF_DEFAULT,
    maxDelay     = OOD_DELAY_DEFAULT; // ms

// internal FIFO buffer — newest at bufHead side, oldest at bufTail side
static PacketNode oodHeadNode = {0}, oodTailNode = {0};
static PacketNode *bufHead = &oodHeadNode, *bufTail = &oodTailNode;
static int bufSize = 0;

static INLINE_FUNCTION short isBufEmpty() {
    short ret = bufHead->next == bufTail;
    if (ret) assert(bufSize == 0);
    return ret;
}

// Collect all buffered packets, Fisher-Yates shuffle, inject into main queue.
// Packets inserted at head side; sendAllListPackets() drains from tail side,
// so delivery order = reverse of injected order = another valid permutation.
static void releaseBufShuffled(PacketNode *head) {
    // OOD_BUF_MAX == 50, so stack array is fine
    PacketNode *arr[50];
    int i, count = bufSize;
    PacketNode *p;

    if (count == 0) return;

    // collect oldest-first from buffer
    p = bufTail->prev;
    for (i = 0; i < count; i++) {
        PacketNode *prev = p->prev;
        arr[i] = popNode(p);
        p = prev;
    }
    bufSize = 0;

    // Fisher-Yates shuffle
    for (i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        PacketNode *tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
    }

    // inject after head — each successive insert pushes previous deeper,
    // so final send order (tail→head) = arr[0] last, arr[count-1] first
    for (i = 0; i < count; i++) {
        insertAfter(arr[i], head);
    }

    LOG("ood: released %d packets shuffled", count);
}

static void oodStartUp() {
    if (bufHead->next == NULL && bufTail->next == NULL) {
        bufHead->next = bufTail;
        bufTail->prev = bufHead;
        bufSize = 0;
    } else {
        assert(isBufEmpty());
    }
    startTimePeriod();
    LOG("ood enabled, buf=%d delay=%dms", maxBuffer, maxDelay);
}

static void oodCloseDown(PacketNode *head, PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    UNREFERENCED_PARAMETER(head);
    // flush in insertion order (no shuffle on close — don't confuse shutdown)
    LOG("ood closing, flushing %d buffered packets", bufSize);
    while (!isBufEmpty()) {
        insertAfter(popNode(bufTail->prev), oldLast);
        --bufSize;
    }
    endTimePeriod();
    LOG("ood disabled");
}

static short oodProcess(PacketNode *head, PacketNode *tail) {
    DWORD now = timeGetTime();
    PacketNode *pac, *prevPac;
    short buf = maxBuffer, del = maxDelay; // snapshot volatiles

    // capture matching packets into buffer
    pac = tail->prev;
    while (pac != head) {
        prevPac = pac->prev;
        if (bufSize < buf
            && checkDirection(pac->meta.outbound, oodInbound, oodOutbound)
            && calcChance(chance)) {
            insertAfter(popNode(pac), bufHead)->timestamp = now;
            ++bufSize;
            InterlockedIncrement(&oodModule.affectedCount);
            LOG("ood: captured packet, buf=%d/%d", bufSize, buf);
        }
        pac = prevPac;
    }

    // release trigger: buffer full OR oldest packet timed out
    if (!isBufEmpty()) {
        DWORD oldest_age = now - bufTail->prev->timestamp;
        if (bufSize >= buf || oldest_age >= (DWORD)del) {
            releaseBufShuffled(head);
            return TRUE;
        }
    }

    return bufSize > 0;
}

static int oodSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&chance, clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-buffer") == 0) {
        InterlockedExchange16(&maxBuffer, clampShort(atoi(value), 2, 50));
        return 1;
    }
    if (strcmp(key, NAME"-delay") == 0) {
        InterlockedExchange16(&maxDelay, clampShort(atoi(value), 10, 2000));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&oodInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&oodOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int oodGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 5) return 0;
    strcpy(kv[n].key, NAME"-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f); n++;
    strcpy(kv[n].key, NAME"-buffer");
    sprintf(kv[n].val, "%d", (int)maxBuffer); n++;
    strcpy(kv[n].key, NAME"-delay");
    sprintf(kv[n].val, "%d", (int)maxDelay); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, oodInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, oodOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec oodParamSpecs[] = {
    { NAME"-inbound",  "Inbound",     "bool",    0, 0 },
    { NAME"-outbound", "Outbound",    "bool",    0, 0 },
    { NAME"-chance",   "Chance (%)",  "percent", 0, 100 },
    { NAME"-buffer",   "Buffer",      "int",     2, 50 },
    { NAME"-delay",    "Delay (ms)",  "int",     10, 2000 },
};

Module oodModule = {
    "Out of order",
    NAME,
    (short*)&oodEnabled,
    oodStartUp,
    oodCloseDown,
    oodProcess,
    oodSetParam,
    oodGetParams,
    oodParamSpecs,
    (int)(sizeof(oodParamSpecs) / sizeof(oodParamSpecs[0])),
    // runtime fields
    0, 0, 0
};
