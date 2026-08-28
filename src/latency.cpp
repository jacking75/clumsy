// Latency histogram  (T8)
//
// "Average delay" hides exactly the behaviour that gets reported as a bug: the
// occasional packet that takes ten times longer than the rest. A histogram
// gives p50/p95/p99 for the cost of one increment per delayed packet, and
// unlike a sample buffer it does not grow with session length.
//
// Who writes: lag.cpp and jitter.cpp, from their process() functions, which
// the divert threads call while holding the capture mutex. Writes are
// therefore already serialised against each other. Who reads: HTTP worker
// threads (/api/stats, /metrics) and the report renderer.
//
// latencyRecord() stays lock-free: the counters are LONG and updated through
// the Interlocked helpers, so a reader always sees a whole value - never a
// torn one - and at worst a histogram from a few microseconds ago. That is
// the right trade for a statistics counter that must not slow the packet path
// down, and a concurrent record can only ever *add* to a bucket, which no
// reader is confused by.
//
// latencyReset() is the exception and takes latencyLock, which every reader
// takes too. A reset empties the buckets, and a reader that snapshotted a
// non-zero total and then watched them go to zero underneath it would walk off
// the end of its search and report p99 = 0ms for a session that had real
// delays in it. Resets happen once per capture, so the lock costs nothing
// where it matters.
//
// The bucket bounds are geometric rather than linear so the same histogram
// resolves a 2ms jitter setting and a 15s lag setting without reconfiguration.

#include <string.h>

#include "common.h"

// Upper bound (inclusive) of each bucket in ms. The final bucket is everything
// above the last bound and is reported with an upper bound of 0 = unbounded.
static const LONG bucketUpper[LATENCY_BUCKETS] = {
    1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 0
};

static volatile LONG buckets[LATENCY_BUCKETS] = {0};
static volatile LONG totalCount = 0;
static volatile LONG minMs      = 0;
static volatile LONG maxMs      = 0;

// The running sum would overflow a 32-bit counter after roughly an hour of
// heavy lag (2^31 ms). There is no 64-bit Interlocked in the platform layer,
// so it is kept as a value below 1e9 plus a count of the 1e9 rollovers - two
// ordinary LONGs, each of which a reader can load atomically.
#define SUM_ROLLOVER 1000000000L
static volatile LONG sumLow  = 0;
static volatile LONG sumHigh = 0;

static CRITICAL_SECTION latencyLock;
static volatile short   latencyLockReady = 0;

void latencyInit(void) {
    if (latencyLockReady) return;
    InitializeCriticalSection(&latencyLock);
    latencyLockReady = 1;
}

// Every reader is short, and all of them run before latencyInit() during
// argument parsing; these two keep that case from being spelled out at each
// call site.
static INLINE_FUNCTION void latencyLockEnter(void) {
    if (latencyLockReady) EnterCriticalSection(&latencyLock);
}
static INLINE_FUNCTION void latencyLockLeave(void) {
    if (latencyLockReady) LeaveCriticalSection(&latencyLock);
}

void latencyReset(void) {
    int i;
    latencyLockEnter();
    for (i = 0; i < LATENCY_BUCKETS; ++i) InterlockedExchange(&buckets[i], 0);
    InterlockedExchange(&totalCount, 0);
    InterlockedExchange(&minMs, 0);
    InterlockedExchange(&maxMs, 0);
    InterlockedExchange(&sumLow, 0);
    InterlockedExchange(&sumHigh, 0);
    latencyLockLeave();
}

void latencyRecord(DWORD delayMs) {
    LONG ms = (LONG)delayMs;
    LONG low;
    int i;

    // A tick counter that wrapped, or a clock that went backwards, would show
    // up as a huge positive delay. Drop it rather than poisoning the tail.
    if (ms < 0 || ms > 3600000L) return;

    for (i = 0; i < LATENCY_BUCKETS - 1; ++i) {
        if (ms <= bucketUpper[i]) break;
    }
    InterlockedIncrement(&buckets[i]);

    if (totalCount == 0 || ms < minMs) InterlockedExchange(&minMs, ms);
    if (ms > maxMs)                    InterlockedExchange(&maxMs, ms);

    // Writers are serialised by the capture mutex, so read-modify-write here
    // needs no CAS loop.
    low = sumLow + ms;
    if (low >= SUM_ROLLOVER) {
        low -= SUM_ROLLOVER;
        InterlockedIncrement(&sumHigh);
    }
    InterlockedExchange(&sumLow, low);

    InterlockedIncrement(&totalCount);
}

LONG latencyCount(void) { return totalCount; }

LONG latencyMin(void) {
    LONG v;
    latencyLockEnter();
    v = totalCount ? minMs : 0;
    latencyLockLeave();
    return v;
}

LONG latencyMax(void) { return maxMs; }

LONG latencyBucketUpper(int ix) {
    if (ix < 0 || ix >= LATENCY_BUCKETS) return 0;
    return bucketUpper[ix];
}

LONG latencyBucketCount(int ix) {
    LONG v;
    if (ix < 0 || ix >= LATENCY_BUCKETS) return 0;
    latencyLockEnter();
    v = buckets[ix];
    latencyLockLeave();
    return v;
}

// sumHigh and sumLow only mean anything together, so a reset landing between
// the two loads would produce a total that never existed.
double latencySumMs(void) {
    double v;
    latencyLockEnter();
    v = (double)sumHigh * (double)SUM_ROLLOVER + (double)sumLow;
    latencyLockLeave();
    return v;
}

double latencyMean(void) {
    LONG n;
    double sum;
    latencyLockEnter();
    n   = totalCount;
    sum = (double)sumHigh * (double)SUM_ROLLOVER + (double)sumLow;
    latencyLockLeave();
    if (n <= 0) return 0.0;
    return sum / (double)n;
}

// Linear interpolation inside whichever bucket contains the requested rank.
// The result is an estimate bounded by the enclosing bucket - the same
// approximation Prometheus histogram_quantile() makes, and for the same
// reason: keeping every sample would cost unbounded memory.
//
// One refinement over the textbook version: the true min and max are recorded
// exactly, so the outermost occupied buckets are narrowed to them. Without it
// a run of 0-400ms delays reports p95 = 467ms, because the bucket holding the
// answer nominally runs to 500 and the interpolation spreads the samples over
// a range a third of which never contained any. Both clamps are no-ops for
// interior buckets, where min is already below lo and max already above hi.
double latencyPercentile(double pct) {
    double target, cumulative = 0.0, result;
    LONG n, lowest, highest;
    int i;

    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    // Everything the walk needs is read under one lock, so the histogram it
    // works from is the histogram that existed at a single instant.
    latencyLockEnter();
    n       = totalCount;
    lowest  = minMs;
    highest = maxMs;
    if (n <= 0) {
        latencyLockLeave();
        return 0.0;
    }
    target = (double)n * pct / 100.0;
    result = (double)highest;

    for (i = 0; i < LATENCY_BUCKETS; ++i) {
        double count = (double)buckets[i];
        if (count <= 0.0) continue;
        if (cumulative + count >= target) {
            double lo = (i == 0) ? 0.0 : (double)bucketUpper[i - 1];
            // The open-ended last bucket has no nominal upper edge at all, so
            // the largest delay seen is the only one available.
            double hi = (bucketUpper[i] != 0) ? (double)bucketUpper[i] : (double)highest;
            double frac;
            if (lo < (double)lowest)  lo = (double)lowest;
            if (hi > (double)highest) hi = (double)highest;
            if (hi < lo) hi = lo;
            frac = (target - cumulative) / count;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            result = lo + frac * (hi - lo);
            break;
        }
        cumulative += count;
    }
    latencyLockLeave();
    return result;
}
