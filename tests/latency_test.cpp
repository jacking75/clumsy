// Unit test for the latency histogram  (T8)
//
// The percentiles are read off bucket counts, not a sample list, so the
// interpolation is the part that can silently be wrong: it looks plausible for
// any input and is only checkable against a distribution whose answer is known
// in advance. This drives known inputs through latencyRecord() and compares
// against the value computed exactly.
//
// Built and run by `make test`.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "common.h"

// latency.cpp is the only thing under test; these satisfy the linker without
// pulling in the rest of clumsy.
volatile short logVerbose = 0;
void logPrintf(const char *fmt, ...) { (void)fmt; }

static int failures = 0;

static void check(const char *what, int cond, const char *detail) {
    printf("%-46s %s", what, cond ? "PASS" : "FAIL");
    if (!cond) { printf("   <- %s", detail ? detail : ""); failures++; }
    printf("\n");
}

static void checkNear(const char *what, double got, double want, double tol) {
    char detail[128];
    snprintf(detail, sizeof(detail), "got %.2f, wanted %.2f +/- %.2f", got, want, tol);
    check(what, fabs(got - want) <= tol, detail);
}

int main(void) {
    printf("clumsy latency histogram\n");
    printf("--------------------------------------------------------\n");

    // --- empty ---
    latencyReset();
    check("empty: count is 0", latencyCount() == 0, NULL);
    check("empty: p50 is 0", latencyPercentile(50) == 0.0, NULL);
    check("empty: mean is 0", latencyMean() == 0.0, NULL);
    check("empty: min is 0", latencyMin() == 0, NULL);

    // --- one sample ---
    latencyReset();
    latencyRecord(42);
    check("single: count is 1", latencyCount() == 1, NULL);
    check("single: min is 42", latencyMin() == 42, NULL);
    check("single: max is 42", latencyMax() == 42, NULL);
    checkNear("single: mean is exactly 42", latencyMean(), 42.0, 0.001);
    // 42 lands in the (25, 50] bucket, so the estimate is bounded by it.
    double p50 = latencyPercentile(50);
    check("single: p50 inside its bucket (25,50]", p50 >= 25.0 && p50 <= 50.0, NULL);

    // --- uniform 0..400, the case the behaviour test exercises ---
    // True median is 200. The buckets straddling it are (100,250] and (250,500],
    // so a correct interpolation must still land on 200, not on a bucket edge.
    latencyReset();
    for (int i = 0; i <= 400; ++i) latencyRecord((DWORD)i);
    check("uniform: 401 samples recorded", latencyCount() == 401, NULL);
    check("uniform: min 0", latencyMin() == 0, NULL);
    check("uniform: max 400", latencyMax() == 400, NULL);
    checkNear("uniform: mean is 200", latencyMean(), 200.0, 0.5);
    checkNear("uniform: p50 recovers the true median", latencyPercentile(50), 200.0, 12.0);
    checkNear("uniform: p25 recovers the true quartile", latencyPercentile(25), 100.0, 12.0);
    checkNear("uniform: p95 recovers the true 95th", latencyPercentile(95), 380.0, 25.0);
    check("uniform: p100 is the max", latencyPercentile(100) >= 399.0, NULL);
    check("uniform: p0 is at the floor", latencyPercentile(0) <= 1.0, NULL);

    // --- monotonicity: percentiles must never go backwards ---
    {
        int mono = 1;
        double prev = -1.0;
        for (int q = 0; q <= 100; q += 5) {
            double v = latencyPercentile(q);
            if (v < prev - 0.001) mono = 0;
            prev = v;
        }
        check("uniform: percentiles are non-decreasing", mono, NULL);
    }

    // --- bucket totals must equal the sample count ---
    {
        long total = 0;
        for (int i = 0; i < LATENCY_BUCKETS; ++i) total += latencyBucketCount(i);
        char d[64];
        snprintf(d, sizeof(d), "buckets sum to %ld, count is %ld", total, (long)latencyCount());
        check("uniform: buckets sum to the total count", total == latencyCount(), d);
    }

    // --- a long tail must move the upper percentiles, not the median ---
    // 5% spike, so the 99th percentile lands inside it while the 90th does not.
    // (With a 1% spike p99 is the *body* value by definition - the rank falls
    // on the last body sample - which is correct but tests nothing.)
    latencyReset();
    for (int i = 0; i < 950; ++i) latencyRecord(10);
    for (int i = 0; i < 50;  ++i) latencyRecord(4000);
    checkNear("tail: p50 stays at the body", latencyPercentile(50), 10.0, 6.0);
    check("tail: p90 is still the body", latencyPercentile(90) <= 25.0, NULL);
    check("tail: p99 reaches the spike", latencyPercentile(99) > 500.0, NULL);
    check("tail: max is the spike", latencyMax() == 4000, NULL);
    check("tail: min is the body", latencyMin() == 10, NULL);
    checkNear("tail: mean is pulled up by the spike", latencyMean(),
              (950 * 10 + 50 * 4000) / 1000.0, 0.01);

    // --- a narrow band must not be reported wider than it is ---
    // Every sample is 300-320ms, all inside the single (250,500] bucket. The
    // min/max clamp is what keeps the answer inside the real range instead of
    // spreading it across the nominal 250ms width of that bucket.
    latencyReset();
    for (int i = 300; i <= 320; ++i) latencyRecord((DWORD)i);
    check("narrow: p50 stays inside the observed range",
          latencyPercentile(50) >= 300.0 && latencyPercentile(50) <= 320.0, NULL);
    check("narrow: p95 stays inside the observed range",
          latencyPercentile(95) >= 300.0 && latencyPercentile(95) <= 320.0, NULL);

    // --- the two-LONG sum must survive crossing 1e9 ms ---
    // 400,000 samples of 3000ms is 1.2e9 ms, past the point a single 32-bit
    // accumulator would wrap into a negative number.
    latencyReset();
    for (int i = 0; i < 400000; ++i) latencyRecord(3000);
    checkNear("rollover: sum survives passing 1e9 ms", latencySumMs(), 1.2e9, 1.0);
    checkNear("rollover: mean is still exact", latencyMean(), 3000.0, 0.001);
    check("rollover: count is right", latencyCount() == 400000, NULL);

    // --- out-of-range input is dropped, not recorded ---
    latencyReset();
    latencyRecord(100);
    latencyRecord(0xFFFFFF00u);   // a wrapped tick counter
    check("guard: an absurd delay is ignored", latencyCount() == 1, NULL);
    check("guard: max is unaffected by it", latencyMax() == 100, NULL);

    // --- zero is a legitimate measurement ---
    latencyReset();
    latencyRecord(0);
    check("zero: a 0ms delay still counts", latencyCount() == 1, NULL);
    check("zero: it lands in the first bucket", latencyBucketCount(0) == 1, NULL);

    printf("--------------------------------------------------------\n");
    if (failures == 0) printf("ALL PASS (0 failures)\n");
    else               printf("%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
