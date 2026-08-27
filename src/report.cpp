// HTML session report  (Phase 3.3)
//
// A test session leaves behind CSV/JSON logs that are fine for machines and
// useless for a stand-up. This renders one self-contained HTML page — filter,
// module timeline, totals, and an inline SVG throughput graph — with no
// external assets, so it can be attached to a ticket and opened anywhere.
//
// Samples are taken by the main tick loop through reportTick(); events are
// appended whenever a scenario step changes something. GET /api/report renders
// on an HTTP worker thread, so the sample and event arrays are guarded by
// reportLock - otherwise a render could read a half-written event.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"

#define MAX_SAMPLES  600    // 600 samples * 1s = 10 minutes at full resolution
#define MAX_EVENTS   256
#define EVENT_TEXT   192

typedef struct {
    DWORD elapsedMs;
    LONG  captured;
    LONG  sent;
} ReportSample;

typedef struct {
    DWORD elapsedMs;
    char  text[EVENT_TEXT];
} SessionEvent;   // not 'ReportEvent': Windows already defines that as a macro

static CRITICAL_SECTION reportLock;
static volatile short   reportLockReady = 0;

// Called once from main() before any thread exists. Lazy first-use creation
// would itself race between the HTTP and main threads.
void reportInit(void) {
    if (reportLockReady) return;
    InitializeCriticalSection(&reportLock);
    reportLockReady = 1;
}

static ReportSample samples[MAX_SAMPLES];
static int          sampleCount = 0;
static SessionEvent events[MAX_EVENTS];
static int          eventCount = 0;

static DWORD  sessionStartTick = 0;
static time_t sessionStartTime = 0;
static DWORD  sessionEndTick   = 0;
static DWORD  lastSampleTick   = 0;
static int    sessionActive    = 0;
static char   sessionFilter[FILTER_BUFSIZE] = "";
static LONG   finalCaptured = 0, finalSent = 0;
static LONG   finalAffected[MODULE_CNT] = {0};

// One sample per second keeps a 10 minute session inside MAX_SAMPLES; longer
// runs simply stop adding points rather than reallocating.
#define SAMPLE_INTERVAL_MS 1000

void reportSessionStart(const char *filterText) {
    if (!reportLockReady) return;
    EnterCriticalSection(&reportLock);
    sampleCount = 0;
    eventCount  = 0;
    sessionStartTick = GetTickCount();
    sessionStartTime = time(NULL);
    lastSampleTick   = sessionStartTick;
    sessionEndTick   = 0;
    sessionActive    = 1;
    finalCaptured = finalSent = 0;
    memset(finalAffected, 0, sizeof(finalAffected));

    strncpy(sessionFilter, filterText ? filterText : "", FILTER_BUFSIZE - 1);
    sessionFilter[FILTER_BUFSIZE - 1] = '\0';
    LeaveCriticalSection(&reportLock);

    reportNoteEvent("session started");
}

void reportSessionStop(void) {
    int ix;
    if (!sessionActive || !reportLockReady) return;
    reportNoteEvent("session stopped");

    EnterCriticalSection(&reportLock);
    sessionEndTick = GetTickCount();
    finalCaptured  = statsCapturedTotal;
    finalSent      = statsSentTotal;
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        finalAffected[ix] = modules[ix]->affectedCount;
    }
    sessionActive = 0;
    LeaveCriticalSection(&reportLock);
}

void reportNoteEvent(const char *text) {
    if (!text || !reportLockReady) return;
    EnterCriticalSection(&reportLock);
    if (eventCount < MAX_EVENTS) {
        events[eventCount].elapsedMs =
            sessionStartTick ? (GetTickCount() - sessionStartTick) : 0;
        strncpy(events[eventCount].text, text, EVENT_TEXT - 1);
        events[eventCount].text[EVENT_TEXT - 1] = '\0';
        eventCount++;
    }
    LeaveCriticalSection(&reportLock);
}

// Called once per main-loop tick while capturing.
void reportTick(void) {
    DWORD now;
    if (!sessionActive || sampleCount >= MAX_SAMPLES) return;
    now = GetTickCount();
    if ((now - lastSampleTick) < SAMPLE_INTERVAL_MS) return;
    if (!reportLockReady) return;

    EnterCriticalSection(&reportLock);
    if (sessionActive && sampleCount < MAX_SAMPLES) {
        lastSampleTick = now;
        samples[sampleCount].elapsedMs = now - sessionStartTick;
        samples[sampleCount].captured  = statsCapturedTotal;
        samples[sampleCount].sent      = statsSentTotal;
        sampleCount++;
    }
    LeaveCriticalSection(&reportLock);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static int htmlEscapeInto(const char *src, char *dst, int dstSize) {
    int i = 0;
    if (!src) { if (dstSize > 0) dst[0] = '\0'; return 0; }
    while (*src && i < dstSize - 7) {
        switch (*src) {
        case '<': memcpy(dst + i, "&lt;",   4); i += 4; break;
        case '>': memcpy(dst + i, "&gt;",   4); i += 4; break;
        case '&': memcpy(dst + i, "&amp;",  5); i += 5; break;
        case '"': memcpy(dst + i, "&quot;", 6); i += 6; break;
        default:  dst[i++] = *src; break;
        }
        src++;
    }
    dst[i] = '\0';
    return i;
}

// Appends into buf with bounds checking; returns the new write position.
static int appendf(char *buf, int bufSize, int pos, const char *fmt, ...) {
    va_list args;
    int n;
    if (pos >= bufSize - 1) return pos;
    va_start(args, fmt);
    n = vsnprintf(buf + pos, bufSize - pos, fmt, args);
    va_end(args);
    if (n < 0) return pos;
    if (n > bufSize - pos - 1) n = bufSize - pos - 1;
    return pos + n;
}

// Inline SVG line chart of packets/sec, computed from the cumulative samples.
static int renderChart(char *buf, int bufSize, int pos) {
    const int W = 900, H = 220, PAD = 36;
    int i;
    long maxPps = 1;

    if (sampleCount < 2) {
        return appendf(buf, bufSize, pos,
            "<p class=\"muted\">Not enough samples to draw a throughput graph "
            "(the session was shorter than two seconds).</p>\n");
    }

    for (i = 1; i < sampleCount; ++i) {
        DWORD dt = samples[i].elapsedMs - samples[i-1].elapsedMs;
        long  d  = samples[i].captured - samples[i-1].captured;
        long  pps = dt > 0 ? (long)((double)d * 1000.0 / dt) : 0;
        if (pps > maxPps) maxPps = pps;
    }

    pos = appendf(buf, bufSize, pos,
        "<svg viewBox=\"0 0 %d %d\" width=\"100%%\" role=\"img\" "
        "aria-label=\"captured packets per second over time\">\n"
        "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"%d\" fill=\"#fbfbfd\" stroke=\"#e2e2ea\"/>\n"
        "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#c8c8d4\"/>\n"
        "<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#c8c8d4\"/>\n"
        "<text x=\"4\" y=\"14\" font-size=\"11\" fill=\"#666\">%ld pkt/s</text>\n"
        "<polyline fill=\"none\" stroke=\"#3b76d8\" stroke-width=\"2\" points=\"",
        W, H, W, H,
        PAD, H - PAD, W - 8, H - PAD,
        PAD, 8, PAD, H - PAD,
        maxPps);

    {
        DWORD span = samples[sampleCount-1].elapsedMs - samples[0].elapsedMs;
        if (span == 0) span = 1;
        for (i = 1; i < sampleCount; ++i) {
            DWORD dt = samples[i].elapsedMs - samples[i-1].elapsedMs;
            long  d  = samples[i].captured - samples[i-1].captured;
            long  pps = dt > 0 ? (long)((double)d * 1000.0 / dt) : 0;
            double x = PAD + (double)(samples[i].elapsedMs - samples[0].elapsedMs)
                             / span * (W - PAD - 8);
            double y = (H - PAD) - ((double)pps / maxPps) * (H - PAD - 8);
            pos = appendf(buf, bufSize, pos, "%.1f,%.1f ", x, y);
        }
    }

    pos = appendf(buf, bufSize, pos,
        "\"/>\n<text x=\"%d\" y=\"%d\" font-size=\"11\" fill=\"#666\">0s</text>\n"
        "<text x=\"%d\" y=\"%d\" font-size=\"11\" fill=\"#666\" text-anchor=\"end\">%lus</text>\n"
        "</svg>\n",
        PAD, H - PAD + 16,
        W - 8, H - PAD + 16,
        (unsigned long)(samples[sampleCount-1].elapsedMs / 1000));
    return pos;
}

// Renders the whole page under reportLock so an in-flight reportTick() cannot
// grow the arrays midway through.
static int renderLocked(char *buf, int bufSize) {
    int pos = 0, ix, i;
    char esc[FILTER_BUFSIZE * 2];
    char timeBuf[64];
    DWORD durationMs;
    LONG captured, sent;

    if (!buf || bufSize < 4096) return 0;

    // While a session is still running, report on live counters.
    captured = sessionActive ? statsCapturedTotal : finalCaptured;
    sent     = sessionActive ? statsSentTotal     : finalSent;
    durationMs = sessionActive
        ? (sessionStartTick ? GetTickCount() - sessionStartTick : 0)
        : (sessionEndTick > sessionStartTick ? sessionEndTick - sessionStartTick : 0);

    if (sessionStartTime) {
        struct tm *lt = localtime(&sessionStartTime);
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", lt);
    } else {
        strcpy(timeBuf, "(no session recorded)");
    }

    pos = appendf(buf, bufSize, pos,
"<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>clumsy session report</title>\n<style>\n"
"  :root{color-scheme:light dark;--bg:#fff;--fg:#1c1c22;--muted:#6b6b78;--line:#e2e2ea;--accent:#3b76d8}\n"
"  @media (prefers-color-scheme:dark){:root{--bg:#16161a;--fg:#e8e8ee;--muted:#9a9aa8;--line:#2c2c34}}\n"
"  body{margin:0;padding:2rem;background:var(--bg);color:var(--fg);\n"
"       font:14px/1.6 system-ui,-apple-system,'Segoe UI',sans-serif;max-width:64rem}\n"
"  h1{font-size:1.4rem;margin:0 0 .25rem}\n"
"  h2{font-size:1.05rem;margin:2rem 0 .5rem;border-bottom:1px solid var(--line);padding-bottom:.25rem}\n"
"  .muted{color:var(--muted)}\n"
"  code{background:rgba(128,128,128,.14);padding:.1rem .35rem;border-radius:3px;\n"
"       font-family:ui-monospace,Consolas,monospace}\n"
"  table{border-collapse:collapse;width:100%%;margin:.5rem 0}\n"
"  th,td{text-align:left;padding:.35rem .6rem;border-bottom:1px solid var(--line)}\n"
"  th{font-weight:600;color:var(--muted);font-size:.85rem}\n"
"  td.num{text-align:right;font-variant-numeric:tabular-nums}\n"
"  .cards{display:flex;flex-wrap:wrap;gap:.75rem;margin:.5rem 0}\n"
"  .card{border:1px solid var(--line);border-radius:6px;padding:.6rem .9rem;min-width:9rem}\n"
"  .card b{display:block;font-size:1.3rem;font-variant-numeric:tabular-nums}\n"
"  .wrap{overflow-x:auto}\n"
"</style></head><body>\n");

    htmlEscapeInto(sessionFilter, esc, sizeof(esc));
    pos = appendf(buf, bufSize, pos,
        "<h1>clumsy session report</h1>\n"
        "<p class=\"muted\">clumsy %s &middot; started %s &middot; duration %lus</p>\n"
        "<h2>Capture</h2>\n<p>Filter: <code>%s</code></p>\n",
        CLUMSY_VERSION, timeBuf, (unsigned long)(durationMs / 1000),
        esc[0] ? esc : "(none)");

    pos = appendf(buf, bufSize, pos,
        "<div class=\"cards\">"
        "<div class=\"card\"><span class=\"muted\">Captured</span><b>%ld</b></div>"
        "<div class=\"card\"><span class=\"muted\">Sent</span><b>%ld</b></div>"
        "<div class=\"card\"><span class=\"muted\">Avg pkt/s</span><b>%ld</b></div>"
        "</div>\n",
        captured, sent,
        durationMs > 0 ? (long)((double)captured * 1000.0 / durationMs) : 0);

    pos = appendf(buf, bufSize, pos, "<h2>Throughput</h2>\n<div class=\"wrap\">\n");
    pos = renderChart(buf, bufSize, pos);
    pos = appendf(buf, bufSize, pos, "</div>\n");

    // --- module table ---
    pos = appendf(buf, bufSize, pos,
        "<h2>Modules</h2>\n<div class=\"wrap\"><table>\n"
        "<tr><th>Module</th><th>Enabled at end</th><th class=\"num\">Packets affected</th>"
        "<th>Parameters</th></tr>\n");
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        Module *m = modules[ix];
        ParamKV kv[32];
        int n = m->getParams ? m->getParams(kv, 32) : 0;
        LONG affected = sessionActive ? m->affectedCount : finalAffected[ix];

        pos = appendf(buf, bufSize, pos,
            "<tr><td>%s</td><td>%s</td><td class=\"num\">%ld</td><td class=\"muted\">",
            m->displayName, *m->enabledFlag ? "yes" : "no", affected);
        for (i = 0; i < n; ++i) {
            pos = appendf(buf, bufSize, pos, "%s%s=%s",
                          i ? ", " : "", kv[i].key, kv[i].val);
        }
        pos = appendf(buf, bufSize, pos, "</td></tr>\n");
    }
    pos = appendf(buf, bufSize, pos, "</table></div>\n");

    // --- timeline ---
    pos = appendf(buf, bufSize, pos,
        "<h2>Timeline</h2>\n<div class=\"wrap\"><table>\n"
        "<tr><th class=\"num\">t</th><th>Event</th></tr>\n");
    if (eventCount == 0) {
        pos = appendf(buf, bufSize, pos,
            "<tr><td colspan=\"2\" class=\"muted\">no events recorded</td></tr>\n");
    }
    for (i = 0; i < eventCount; ++i) {
        char etext[EVENT_TEXT * 2];
        htmlEscapeInto(events[i].text, etext, sizeof(etext));
        pos = appendf(buf, bufSize, pos,
            "<tr><td class=\"num\">%lu.%03lus</td><td>%s</td></tr>\n",
            (unsigned long)(events[i].elapsedMs / 1000),
            (unsigned long)(events[i].elapsedMs % 1000),
            etext);
    }
    pos = appendf(buf, bufSize, pos, "</table></div>\n");

    if (pcapExportPath()[0]) {
        char pesc[MSG_BUFSIZE * 2];
        htmlEscapeInto(pcapExportPath(), pesc, sizeof(pesc));
        pos = appendf(buf, bufSize, pos,
            "<h2>Packet capture</h2>\n<p>%ld packets written to <code>%s</code>.</p>\n",
            pcapExportCount(), pesc);
    }

    pos = appendf(buf, bufSize, pos,
        "<p class=\"muted\" style=\"margin-top:2rem\">Generated by clumsy %s.</p>\n"
        "</body></html>\n", CLUMSY_VERSION);

    return pos;
}

int reportRenderHtml(char *buf, int bufSize) {
    int n;
    if (!buf || bufSize < 4096 || !reportLockReady) return 0;
    EnterCriticalSection(&reportLock);
    n = renderLocked(buf, bufSize);
    LeaveCriticalSection(&reportLock);
    return n;
}

int reportWriteHtml(const char *path) {
    char *buf;
    int n;
    FILE *f;

    if (!path || !path[0]) return 0;

    buf = (char*)malloc(512 * 1024);
    if (!buf) return 0;

    n = reportRenderHtml(buf, 512 * 1024);
    if (n <= 0) { free(buf); return 0; }

    f = fopen(path, "wb");
    if (!f) {
        INFO("report: cannot write '%s'", path);
        free(buf);
        return 0;
    }
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    free(buf);
    INFO("report: wrote session report to '%s'", path);
    return 1;
}
