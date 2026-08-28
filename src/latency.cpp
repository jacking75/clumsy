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
// threads (/api/stats, /metrics) and the report renderer, with no lock at all.
// The counters are LONG and updated through the Interlocked helpers, so a
// reader always sees a whole value - never a torn one - and at worst sees a
// histogram from a few microseconds ago. That is the right trade for a
// statistics counter that must not slow the packet path down.
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

void latencyReset(void) {
    int i;
    for (i = 0; i < LATENCY_BUCKETS; ++i) InterlockedExchange(&buckets[i], 0);
    InterlockedExchange(&totalCount, 0);
    InterlockedExchange(&minMs, 0);
    InterlockedExchange(&maxMs, 0);
    InterlockedExchange(&sumLow, 0);
    InterlockedExchange(&sumHigh, 0);
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
LONG latencyMin(void)   { return totalCount ? minMs : 0; }
LONG latencyMax(void)   { return maxMs; }

LONG latencyBucketUpper(int ix) {
    if (ix < 0 || ix >= LATENCY_BUCKETS) return 0;
    return bucketUpper[ix];
}

LONG latencyBucketCount(int ix) {
    if (ix < 0 || ix >= LATENCY_BUCKETS) return 0;
    return buckets[ix];
}

double latencySumMs(void) {
    return (double)sumHigh * (double)SUM_ROLLOVER + (double)sumLow;
}

double latencyMean(void) {
    LONG n = totalCount;
    if (n <= 0) return 0.0;
    return latencySumMs() / (double)n;
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
    LONG n = totalCount;
    double target, cumulative = 0.0;
    int i;

    if (n <= 0) return 0.0;
    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    target = (double)n * pct / 100.0;

    for (i = 0; i < LATENCY_BUCKETS; ++i) {
        double count = (double)buckets[i];
        if (count <= 0.0) continue;
        if (cumulative + count >= target) {
            double lo = (i == 0) ? 0.0 : (double)bucketUpper[i - 1];
            // The open-ended last bucket has no nominal upper edge at all, so
            // the largest delay seen is the only one available.
            double hi = (bucketUpper[i] != 0) ? (double)bucketUpper[i] : (double)maxMs;
            double frac;
            if (lo < (double)minMs) lo = (double)minMs;
            if (hi > (double)maxMs) hi = (double)maxMs;
            if (hi < lo) hi = lo;
            frac = (target - cumulative) / count;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            return lo + frac * (hi - lo);
        }
        cumulative += count;
    }
    return (double)maxMs;
}
