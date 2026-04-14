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
#include "iup.h"
#include "common.h"

#define NAME "ood"
#define OOD_BUF_DEFAULT    5
#define OOD_DELAY_DEFAULT  200
#define OOD_BUF_MIN   "2"
#define OOD_BUF_MAX   "50"
#define OOD_DELAY_MIN "10"
#define OOD_DELAY_MAX "2000"

static Ihandle *inboundCheckbox, *outboundCheckbox;
static Ihandle *chanceInput, *bufInput, *delayInput;

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

static Ihandle* oodSetupUI() {
    Ihandle *oodControlsBox = IupHbox(
        inboundCheckbox  = IupToggle("Inbound",  NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Chance(%):"),
        chanceInput = IupText(NULL),
        IupLabel("Buf:"),
        bufInput    = IupText(NULL),
        IupLabel("Delay(ms):"),
        delayInput  = IupText(NULL),
        NULL
    );

    IupSetAttribute(chanceInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(chanceInput, "VALUE", "10.0");
    IupSetCallback(chanceInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(chanceInput, SYNCED_VALUE, (char*)&chance);

    IupSetAttribute(bufInput, "VISIBLECOLUMNS", "2");
    IupSetAttribute(bufInput, "VALUE", STR(OOD_BUF_DEFAULT));
    IupSetCallback(bufInput, "VALUECHANGED_CB", uiSyncInteger);
    IupSetAttribute(bufInput, SYNCED_VALUE, (char*)&maxBuffer);
    IupSetAttribute(bufInput, INTEGER_MAX, OOD_BUF_MAX);
    IupSetAttribute(bufInput, INTEGER_MIN, OOD_BUF_MIN);

    IupSetAttribute(delayInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(delayInput, "VALUE", STR(OOD_DELAY_DEFAULT));
    IupSetCallback(delayInput, "VALUECHANGED_CB", uiSyncInteger);
    IupSetAttribute(delayInput, SYNCED_VALUE, (char*)&maxDelay);
    IupSetAttribute(delayInput, INTEGER_MAX, OOD_DELAY_MAX);
    IupSetAttribute(delayInput, INTEGER_MIN, OOD_DELAY_MIN);

    IupSetCallback(inboundCheckbox,  "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox,  SYNCED_VALUE, (char*)&oodInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&oodOutbound);

    IupSetAttribute(inboundCheckbox,  "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox,  "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(chanceInput, "VALUE", NAME"-chance");
        setFromParameter(bufInput,    "VALUE", NAME"-buffer");
        setFromParameter(delayInput,  "VALUE", NAME"-delay");
    }

    return oodControlsBox;
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
            && checkDirection(pac->addr.Outbound, oodInbound, oodOutbound)
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
    if (strcmp(key, "ood-chance") == 0) {
        short v = I2S((int)(atof(value) * 100.0 + 0.5));
        char buf[16];
        if (v < 0) v = 0; if (v > 10000) v = 10000;
        InterlockedExchange16(&chance, v);
        sprintf(buf, "%.1f", (float)v / 100.0f);
        if (chanceInput) IupStoreAttribute(chanceInput, "VALUE", buf);
        return 1;
    }
    if (strcmp(key, "ood-buffer") == 0) {
        int v = atoi(value);
        char buf[16];
        if (v < 2) v = 2; if (v > 50) v = 50;
        InterlockedExchange16(&maxBuffer, I2S(v));
        sprintf(buf, "%d", v);
        if (bufInput) IupStoreAttribute(bufInput, "VALUE", buf);
        return 1;
    }
    if (strcmp(key, "ood-delay") == 0) {
        int v = atoi(value);
        char buf[16];
        if (v < 10) v = 10; if (v > 2000) v = 2000;
        InterlockedExchange16(&maxDelay, I2S(v));
        sprintf(buf, "%d", v);
        if (delayInput) IupStoreAttribute(delayInput, "VALUE", buf);
        return 1;
    }
    return 0;
}

static int oodGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 3) return 0;
    strcpy(kv[n].key, "ood-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f);
    n++;
    strcpy(kv[n].key, "ood-buffer");
    sprintf(kv[n].val, "%d", (int)maxBuffer);
    n++;
    strcpy(kv[n].key, "ood-delay");
    sprintf(kv[n].val, "%d", (int)maxDelay);
    n++;
    return n;
}

Module oodModule = {
    "Out of order",
    NAME,
    (short*)&oodEnabled,
    oodSetupUI,
    oodStartUp,
    oodCloseDown,
    oodProcess,
    oodSetParam,
    oodGetParams,
    // runtime fields
    0, 0, NULL, 0
};
