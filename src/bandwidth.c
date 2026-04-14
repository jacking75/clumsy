// bandwidth cap module - token bucket queuing
//
// Previous behavior: packets exceeding the rate limit were immediately dropped.
// New behavior: packets are queued and released when bandwidth becomes available,
// simulating real network buffering. Overflow (queue > KEEP_AT_MOST) uses
// tail-drop as a last resort.
//
// Token bucket:
//   fill rate     = bandwidthLimit KB/s
//   bucket size   = max(65535, fill_rate * BUCKET_SECS)  [burst allowance]
//   on each step  = add (elapsed_ms * fill_rate_bytes_per_ms) tokens, capped
//   release packet = deduct packet_size tokens, send in FIFO order

#include <stdlib.h>
#include <Windows.h>
#include <stdint.h>

#include "iup.h"
#include "common.h"

#define NAME "bandwidth"
#define BANDWIDTH_MIN     "0"
#define BANDWIDTH_MAX     "99999"
#define BANDWIDTH_DEFAULT 10

// max queued packets before overflow tail-drop
#define KEEP_AT_MOST 2000
// token bucket capacity = BUCKET_SECS seconds worth of bandwidth
// (allows a short burst after idle period without penalty)
#define BUCKET_SECS 2

static Ihandle *inboundCheckbox, *outboundCheckbox, *bandwidthInput;

static volatile short bandwidthEnabled  = 0,
    bandwidthInbound  = 1,
    bandwidthOutbound = 1;

static volatile LONG bandwidthLimit = BANDWIDTH_DEFAULT; // KB/s

// FIFO packet queue — same pattern as lag.c
static PacketNode bwHeadNode = {0}, bwTailNode = {0};
static PacketNode *bufHead = &bwHeadNode, *bufTail = &bwTailNode;
static int bufSize = 0;

// token bucket state (only touched inside divert mutex)
static double tokenBucket  = 0.0;
static double maxTokens    = 0.0;
static DWORD  lastFillTime = 0;

static INLINE_FUNCTION short isBufEmpty() {
    short ret = bufHead->next == bufTail;
    if (ret) assert(bufSize == 0);
    return ret;
}

static Ihandle* bandwidthSetupUI() {
    Ihandle *bandwidthControlsBox = IupHbox(
        inboundCheckbox  = IupToggle("Inbound",  NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Limit(KB/s):"),
        bandwidthInput = IupText(NULL),
        NULL
    );

    IupSetAttribute(bandwidthInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(bandwidthInput, "VALUE", STR(BANDWIDTH_DEFAULT));
    IupSetCallback(bandwidthInput, "VALUECHANGED_CB", uiSyncInt32);
    IupSetAttribute(bandwidthInput, SYNCED_VALUE, (char*)&bandwidthLimit);
    IupSetAttribute(bandwidthInput, INTEGER_MAX, BANDWIDTH_MAX);
    IupSetAttribute(bandwidthInput, INTEGER_MIN, BANDWIDTH_MIN);
    IupSetCallback(inboundCheckbox,  "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox,  SYNCED_VALUE, (char*)&bandwidthInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&bandwidthOutbound);

    IupSetAttribute(inboundCheckbox,  "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox,  "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(bandwidthInput,   "VALUE", NAME"-bandwidth");
    }

    return bandwidthControlsBox;
}

static void bandwidthStartUp() {
    LONG limit;

    if (bufHead->next == NULL && bufTail->next == NULL) {
        bufHead->next = bufTail;
        bufTail->prev = bufHead;
        bufSize = 0;
    } else {
        assert(isBufEmpty());
    }

    // initialise token bucket — start full so first burst passes without delay
    limit      = bandwidthLimit;
    maxTokens  = (double)limit * 1024.0 * BUCKET_SECS;
    if (maxTokens < 65535.0) maxTokens = 65535.0;
    tokenBucket  = maxTokens;
    lastFillTime = timeGetTime();

    startTimePeriod();
    LOG("bandwidth enabled, limit=%ldKB/s", limit);
}

static void bandwidthCloseDown(PacketNode *head, PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    UNREFERENCED_PARAMETER(head);
    // flush all buffered packets immediately so nothing is lost on disable
    LOG("bandwidth closing, flushing %d queued packets", bufSize);
    while (!isBufEmpty()) {
        insertAfter(popNode(bufTail->prev), oldLast);
        --bufSize;
    }
    endTimePeriod();
    LOG("bandwidth disabled");
}

static short bandwidthProcess(PacketNode *head, PacketNode *tail) {
    DWORD  now   = timeGetTime();
    LONG   limit = bandwidthLimit;   // snapshot volatile
    double fillRate = (double)limit * 1024.0; // bytes per second
    PacketNode *pac, *prevPac;

    // --- 1. replenish tokens ---
    if (limit > 0) {
        double elapsedSec = (double)(DWORD)(now - lastFillTime) / 1000.0;
        double newMax = fillRate * BUCKET_SECS;
        if (newMax < 65535.0) newMax = 65535.0;
        maxTokens    = newMax;
        tokenBucket += elapsedSec * fillRate;
        if (tokenBucket > maxTokens) tokenBucket = maxTokens;
    }
    lastFillTime = now;

    // --- 2. enqueue incoming packets (tail -> head, oldest first) ---
    pac = tail->prev;
    while (pac != head) {
        prevPac = pac->prev;
        if (checkDirection(pac->addr.Outbound, bandwidthInbound, bandwidthOutbound)) {
            if (bufSize < KEEP_AT_MOST) {
                // insert at bufHead: iterating oldest-first means oldest ends up
                // deepest, so bufTail->prev drains oldest first (FIFO preserved)
                insertAfter(popNode(pac), bufHead);
                ++bufSize;
                InterlockedIncrement(&bandwidthModule.affectedCount);
            } else {
                // buffer full — tail drop
                LOG("bandwidth: queue full (%d), dropping packet", KEEP_AT_MOST);
                freeNode(popNode(pac));
            }
        }
        pac = prevPac;
    }

    // --- 3. drain queue FIFO while tokens allow ---
    if (limit > 0) {
        while (!isBufEmpty()) {
            PacketNode *oldest = bufTail->prev;
            double cost = (double)oldest->packetLen;
            if (tokenBucket >= cost) {
                tokenBucket -= cost;
                insertAfter(popNode(oldest), head);
                --bufSize;
                LOG("bandwidth: released %u-byte packet, %.0f tokens remain",
                    oldest->packetLen, tokenBucket);
            } else {
                break; // not enough tokens yet
            }
        }
    }
    // limit == 0: no fill, queue grows until KEEP_AT_MOST then drops (block-all)

    return bufSize > 0;
}

static int bandwidthSetParam(const char *key, const char *value) {
    if (strcmp(key, "bandwidth-bandwidth") == 0) {
        LONG v = atol(value);
        char buf[16];
        if (v < 0) v = 0; if (v > 99999) v = 99999;
        InterlockedExchange(&bandwidthLimit, v);
        sprintf(buf, "%ld", v);
        if (bandwidthInput) IupStoreAttribute(bandwidthInput, "VALUE", buf);
        return 1;
    }
    return 0;
}

static int bandwidthGetParams(ParamKV *kv, int maxKv) {
    if (maxKv < 1) return 0;
    strcpy(kv[0].key, "bandwidth-bandwidth");
    sprintf(kv[0].val, "%ld", bandwidthLimit);
    return 1;
}

int bandwidthGetBufSize(void) { return bufSize; }
LONG bandwidthGetLimitKBps(void) { return bandwidthLimit; }

Module bandwidthModule = {
    "Bandwidth",
    NAME,
    (short*)&bandwidthEnabled,
    bandwidthSetupUI,
    bandwidthStartUp,
    bandwidthCloseDown,
    bandwidthProcess,
    bandwidthSetParam,
    bandwidthGetParams,
    // runtime fields
    0, 0, NULL, 0
};
