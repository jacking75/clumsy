// jitter module - random delay per packet within [min, max] range
//
// The delay is drawn from a selectable distribution. Real network delay is
// rarely uniform: a healthy link clusters near its mean, and a congested one
// is mostly fine with occasional large spikes. Both shapes matter when
// reproducing a bug, so the same three distributions Linux tc-netem offers
// are available here.
#include <math.h>
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

// Delay distributions, in the order the "jitter-dist" enum exposes them.
#define DIST_UNIFORM 0
#define DIST_NORMAL  1
#define DIST_PARETO  2
#define DIST_NAMES   "uniform,normal,pareto"

static volatile short jitterEnabled = 0,
    jitterInbound = 1,
    jitterOutbound = 1,
    jitterMin = JITTER_MIN_DEFAULT,
    jitterMax = JITTER_MAX_DEFAULT,
    jitterDist = DIST_UNIFORM;

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

// Draws one delay in ms from [lo, hi] under the selected distribution.
//
// Both shaped distributions are scaled so their useful mass lands inside the
// range the operator typed, and anything outside is clamped rather than
// resampled: a rejection loop has no bounded worst case, and clamping only
// touches the far tail beyond the limit the operator already declared.
static DWORD jitterSampleDelay(short lo, short hi, short dist) {
    double range = (double)hi - (double)lo;
    double v;

    if (range <= 0.0) return (DWORD)lo;

    switch (dist) {
    case DIST_NORMAL: {
        // Box-Muller. Centre the mean and take sigma = range/6, so ~99.7% of
        // the draws already fall inside [lo, hi] before any clamping.
        double u1 = randUnit(), u2 = randUnit();
        double z  = sqrt(-2.0 * log(u1)) * cos(2.0 * 3.14159265358979323846 * u2);
        v = ((double)lo + (double)hi) / 2.0 + z * (range / 6.0);
        break;
    }
    case DIST_PARETO: {
        // Long tail: mostly near lo, occasionally out to hi. Shape 2 puts the
        // median near lo + 0.29*range and leaves a visible upper tail.
        // pow() returns [1, inf), so (1 - 1/p) maps it onto [0, 1).
        double p = pow(1.0 - randUnit(), -1.0 / 2.0);
        v = (double)lo + (1.0 - 1.0 / p) * range;
        break;
    }
    default:
        v = (double)lo + randUnit() * range;
        break;
    }

    if (v < (double)lo) v = (double)lo;
    if (v > (double)hi) v = (double)hi;
    return (DWORD)(v + 0.5);
}

static short jitterProcess(PacketNode *head, PacketNode *tail) {
    DWORD currentTime = timeGetTime();
    PacketNode *pac = tail->prev;
    // snapshot volatile values once per call
    short lo = jitterMin, hi = jitterMax, dist = jitterDist;

    // normalize in case user typed min > max
    if (lo > hi) { short tmp = lo; lo = hi; hi = tmp; }

    // move incoming packets into jitter buffer, assign each a random target send time
    while (bufSize < KEEP_AT_MOST && pac != head) {
        if (checkDirection(pac->meta.outbound, jitterInbound, jitterOutbound)) {
            DWORD delay = jitterSampleDelay(lo, hi, dist);
            PacketNode *queued = insertAfter(popNode(pac), bufHead);
            // timestamp is the scheduled release; enqueueTime is what the
            // latency histogram measures against once it actually goes out.
            queued->enqueueTime = currentTime;
            queued->timestamp   = currentTime + delay;
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
            DWORD waited = currentTime - pac->enqueueTime;
            LOG("jitter released packet after %lums", (unsigned long)waited);
            latencyRecord(waited);
            insertAfter(popNode(pac), head);
            --bufSize;
        }
        pac = prevPac;
    }

    // buffer overflow guard: flush oldest packets when full
    if (bufSize >= KEEP_AT_MOST) {
        int flushCnt = FLUSH_WHEN_FULL;
        while (flushCnt-- > 0 && !isBufEmpty()) {
            PacketNode *forced = bufTail->prev;
            latencyRecord(currentTime - forced->enqueueTime);
            insertAfter(popNode(forced), head);
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
    if (strcmp(key, NAME"-dist") == 0) {
        short d = DIST_UNIFORM;
        if      (_stricmp(value, "normal") == 0) d = DIST_NORMAL;
        else if (_stricmp(value, "pareto") == 0) d = DIST_PARETO;
        else if (_stricmp(value, "uniform") != 0) {
            // Also accept the raw index, so a scenario or profile written
            // against the numeric form keeps working.
            int n = atoi(value);
            d = (short)((n >= DIST_UNIFORM && n <= DIST_PARETO) ? n : DIST_UNIFORM);
        }
        InterlockedExchange16(&jitterDist, d);
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

static const char* jitterDistName(short d) {
    switch (d) {
    case DIST_NORMAL: return "normal";
    case DIST_PARETO: return "pareto";
    default:          return "uniform";
    }
}

static int jitterGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 5) return 0;
    strcpy(kv[n].key, NAME"-min");
    sprintf(kv[n].val, "%d", (int)jitterMin); n++;
    strcpy(kv[n].key, NAME"-max");
    sprintf(kv[n].val, "%d", (int)jitterMax); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, jitterInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, jitterOutbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-dist");
    strcpy(kv[n].val, jitterDistName(jitterDist)); n++;
    return n;
}

static const ParamSpec jitterParamSpecs[] = {
    { NAME"-inbound",  "Inbound",      "bool", 0, 0,    NULL },
    { NAME"-outbound", "Outbound",     "bool", 0, 0,    NULL },
    { NAME"-min",      "Min (ms)",     "int",  0, 5000, NULL },
    { NAME"-max",      "Max (ms)",     "int",  0, 5000, NULL },
    { NAME"-dist",     "Distribution", "enum", 0, 0,    DIST_NAMES },
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
