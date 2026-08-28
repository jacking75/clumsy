// statslog.c - periodic CSV/JSON stats logging to file
//
// Activated via CLI:  --stats-log stats.csv  --stats-interval 1
// Writes one row per interval while filtering is active.
//
// Threading: this file has no lock of its own. All three entry points run with
// main.cpp's appLock held - statsLogStart() and statsLogStop() from
// appStartCapture()/appStopCapture(), statsLogTick() from the main tick loop -
// and that is what serialises them, exactly as the synchronisation table in
// CLAUDE.md says. Start and stop both arrive on HTTP and pipe worker threads,
// so opening the file outside the lock would let a concurrent stop close the
// FILE* this thread is still writing to. Keep any new caller inside appLock.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

static FILE *logFile = NULL;
static int   logInterval = 1;   // seconds
static DWORD logLastTick = 0;
static DWORD logStartTick = 0;
static LONG  logPrevCaptured = 0;
static int   logIsJson = 0;     // 0=CSV, 1=JSON (based on extension)
static int   logJsonFirstRow = 1;

static int endsWithIgnoreCase(const char *str, const char *suffix) {
    size_t slen = strlen(str), xlen = strlen(suffix);
    if (slen < xlen) return 0;
    return _stricmp(str + slen - xlen, suffix) == 0;
}

void statsLogStart(const char *path, int intervalSec) {
    if (!path || path[0] == '\0') return;

    // Close any previous run rather than dropping its handle: a JSON log left
    // open would also be missing its closing bracket and unreadable.
    if (logFile) statsLogStop();

    logIsJson = endsWithIgnoreCase(path, ".json");
    logFile = fopen(path, "w");
    if (!logFile) {
        LOG("statslog: failed to open %s", path);
        return;
    }

    logInterval = (intervalSec > 0) ? intervalSec : 1;
    logStartTick = GetTickCount();
    logLastTick = logStartTick;
    logPrevCaptured = statsCapturedTotal;

    if (logIsJson) {
        fprintf(logFile, "[\n");
        logJsonFirstRow = 1;
    } else {
        // CSV header
        fprintf(logFile,
            "elapsed_sec,captured,sent,pps");
        {
            int ix;
            for (ix = 0; ix < MODULE_CNT; ++ix) {
                fprintf(logFile, ",%s", modules[ix]->shortName);
            }
        }
        fprintf(logFile, ",lag_buf,jitter_buf,bw_buf,bw_limit_kbps\n");
    }
    fflush(logFile);

    LOG("statslog: started, file=%s interval=%ds format=%s",
        path, logInterval, logIsJson ? "json" : "csv");
}

void statsLogStop(void) {
    if (!logFile) return;

    if (logIsJson) {
        fprintf(logFile, "\n]\n");
    }

    fclose(logFile);
    logFile = NULL;
    LOG("statslog: stopped");
}

void statsLogTick(void) {
    DWORD now, elapsed;
    LONG captured, sent;
    int pps, ix;

    if (!logFile) return;

    now = GetTickCount();
    elapsed = now - logLastTick;
    if (elapsed < (DWORD)(logInterval * 1000)) return;

    // compute stats snapshot
    captured = statsCapturedTotal;
    sent     = statsSentTotal;
    pps      = (int)((captured - logPrevCaptured) * 1000 / (int)elapsed);
    logPrevCaptured = captured;
    logLastTick = now;

    if (logIsJson) {
        if (!logJsonFirstRow) {
            fprintf(logFile, ",\n");
        }
        logJsonFirstRow = 0;

        fprintf(logFile,
            "  {\"elapsed_sec\":%.1f,\"captured\":%ld,\"sent\":%ld,\"pps\":%d",
            (double)(now - logStartTick) / 1000.0, captured, sent, pps);

        fprintf(logFile, ",\"modules\":{");
        for (ix = 0; ix < MODULE_CNT; ++ix) {
            if (ix > 0) fprintf(logFile, ",");
            fprintf(logFile, "\"%s\":%ld",
                modules[ix]->shortName, modules[ix]->affectedCount);
        }
        fprintf(logFile, "}");

        fprintf(logFile, ",\"lag_buf\":%d,\"jitter_buf\":%d,\"bw_buf\":%d,\"bw_limit_kbps\":%ld}",
            lagGetBufSize(), jitterGetBufSize(), bandwidthGetBufSize(), bandwidthGetLimitKBps());
    } else {
        // CSV row
        fprintf(logFile, "%.1f,%ld,%ld,%d",
            (double)(now - logStartTick) / 1000.0, captured, sent, pps);

        for (ix = 0; ix < MODULE_CNT; ++ix) {
            fprintf(logFile, ",%ld", modules[ix]->affectedCount);
        }

        fprintf(logFile, ",%d,%d,%d,%ld\n",
            lagGetBufSize(), jitterGetBufSize(), bandwidthGetBufSize(), bandwidthGetLimitKBps());
    }

    fflush(logFile);
}
