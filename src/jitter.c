// jitter module - random delay per packet within [min, max] range
#include <stdlib.h>
#include "iup.h"
#include "common.h"

#define NAME "jitter"
#define JITTER_MIN_DEFAULT 20
#define JITTER_MAX_DEFAULT 100
#define JITTER_TIME_MIN "0"
#define JITTER_TIME_MAX "5000"
#define KEEP_AT_MOST 2000
#define FLUSH_WHEN_FULL 800

static Ihandle *inboundCheckbox, *outboundCheckbox, *minInput, *maxInput;

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

static Ihandle *jitterSetupUI() {
    Ihandle *jitterControlsBox = IupHbox(
        inboundCheckbox  = IupToggle("Inbound", NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Min(ms):"),
        minInput = IupText(NULL),
        IupLabel("Max(ms):"),
        maxInput = IupText(NULL),
        NULL
    );

    IupSetAttribute(minInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(minInput, "VALUE", STR(JITTER_MIN_DEFAULT));
    IupSetCallback(minInput, "VALUECHANGED_CB", uiSyncInteger);
    IupSetAttribute(minInput, SYNCED_VALUE, (char*)&jitterMin);
    IupSetAttribute(minInput, INTEGER_MAX, JITTER_TIME_MAX);
    IupSetAttribute(minInput, INTEGER_MIN, JITTER_TIME_MIN);

    IupSetAttribute(maxInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(maxInput, "VALUE", STR(JITTER_MAX_DEFAULT));
    IupSetCallback(maxInput, "VALUECHANGED_CB", uiSyncInteger);
    IupSetAttribute(maxInput, SYNCED_VALUE, (char*)&jitterMax);
    IupSetAttribute(maxInput, INTEGER_MAX, JITTER_TIME_MAX);
    IupSetAttribute(maxInput, INTEGER_MIN, JITTER_TIME_MIN);

    IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char*)&jitterInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&jitterOutbound);

    IupSetAttribute(inboundCheckbox, "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox,  "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(minInput, "VALUE", NAME"-min");
        setFromParameter(maxInput, "VALUE", NAME"-max");
    }

    return jitterControlsBox;
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
        if (checkDirection(pac->addr.Outbound, jitterInbound, jitterOutbound)) {
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
    if (strcmp(key, "jitter-min") == 0) {
        int v = atoi(value);
        char buf[16];
        if (v < 0) v = 0; if (v > 5000) v = 5000;
        InterlockedExchange16(&jitterMin, I2S(v));
        sprintf(buf, "%d", v);
        if (minInput) IupStoreAttribute(minInput, "VALUE", buf);
        return 1;
    }
    if (strcmp(key, "jitter-max") == 0) {
        int v = atoi(value);
        char buf[16];
        if (v < 0) v = 0; if (v > 5000) v = 5000;
        InterlockedExchange16(&jitterMax, I2S(v));
        sprintf(buf, "%d", v);
        if (maxInput) IupStoreAttribute(maxInput, "VALUE", buf);
        return 1;
    }
    return 0;
}

static int jitterGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 2) return 0;
    strcpy(kv[n].key, "jitter-min");
    sprintf(kv[n].val, "%d", (int)jitterMin);
    n++;
    strcpy(kv[n].key, "jitter-max");
    sprintf(kv[n].val, "%d", (int)jitterMax);
    n++;
    return n;
}

int jitterGetBufSize(void) { return bufSize; }

Module jitterModule = {
    "Jitter",
    NAME,
    (short*)&jitterEnabled,
    jitterSetupUI,
    jitterStartUp,
    jitterCloseDown,
    jitterProcess,
    jitterSetParam,
    jitterGetParams,
    // runtime fields
    0, 0, NULL, 0
};
