// clumsy — console entry point.
//
// Phase 2 removed the IUP GUI entirely. What is left here is:
//   * argument parsing and module parameter bootstrapping
//   * filter preset loading (config.json / config.txt)
//   * the application control surface (appStartCapture / appStopCapture / ...)
//     shared by the Named Pipe server, the HTTP server and the scenario runner
//   * the main tick loop that drives scenario playback and stats logging
//
// All user interaction happens over the web dashboard (httpserver.cpp), the
// Named Pipe API (pipe.cpp) or command line arguments.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "common.h"
#include "filterexpr.h"
#include "httpserver.h"

#if !defined(_WIN32)
#  include <csignal>
#  include <sys/file.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

// ! the order decides which module get processed first
Module* modules[MODULE_CNT] = {
    &lagModule,
    &jitterModule,
    &dropModule,
    &burstlossModule,
    &blackoutModule,
    &throttleModule,
    &dupModule,
    &oodModule,
    &tamperModule,
    &corruptModule,
    &resetModule,
    &bandwidthModule,
};

volatile short sendState = SEND_STATUS_NONE;

// ---------------------------------------------------------------------------
// Filter presets — config.json (preferred) and config.txt (legacy)
// ---------------------------------------------------------------------------
#define CONFIG_JSON_FILE "config.json"
#define CONFIG_TXT_FILE  "config.txt"
#define CONFIG_MAX_RECORDS 64
#define CONFIG_BUF_SIZE 8192
#define PRESET_NAME_SIZE 64

// Presets own their strings (rather than pointing into the parse buffer) so
// that appPresetSave() can add one and rewrite the whole file.
typedef struct {
    char name[PRESET_NAME_SIZE];
    char filter[FILTER_BUFSIZE];
} FilterRecord;

static FilterRecord filters[CONFIG_MAX_RECORDS];
static int          filtersSize = 0;
static char         configBuf[CONFIG_BUF_SIZE + 2];

// resolve exe directory and append filename into pathBuf
static void buildConfigPath(char *pathBuf, int bufSize, const char *filename) {
    char *p;
    GetModuleFileName(NULL, pathBuf, bufSize);
    p = strrchr(pathBuf, '\\');
    if (p == NULL) p = strrchr(pathBuf, '/');
    if (p == NULL) { strncpy(pathBuf, filename, bufSize - 1); pathBuf[bufSize-1] = '\0'; return; }
    strcpy(p + 1, filename);
}

static void addPreset(const char *name, const char *filter) {
    if (filtersSize >= CONFIG_MAX_RECORDS) return;
    strncpy(filters[filtersSize].name, name, PRESET_NAME_SIZE - 1);
    filters[filtersSize].name[PRESET_NAME_SIZE - 1] = '\0';
    strncpy(filters[filtersSize].filter, filter, FILTER_BUFSIZE - 1);
    filters[filtersSize].filter[FILTER_BUFSIZE - 1] = '\0';
    filtersSize++;
}

// parse config.json — JSON object with "filters" array of {"name":"...","filter":"..."}
static int loadConfigJson(const char *path) {
    FILE *f;
    size_t len;
    const char *p, *arrEnd;
    int depth;

    f = fopen(path, "r");
    if (!f) return 0;

    len = fread(configBuf, 1, CONFIG_BUF_SIZE, f);
    fclose(f);
    if (len == 0) return 0;
    configBuf[len] = '\0';

    // find "filters" array
    p = strstr(configBuf, "\"filters\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;

    // find matching ']'
    arrEnd = p + 1;
    depth = 1;
    while (*arrEnd && depth > 0) {
        if (*arrEnd == '[') depth++;
        else if (*arrEnd == ']') depth--;
        arrEnd++;
    }

    p++; // skip '['
    filtersSize = 0;

    // iterate each {...} object in the array
    while (p < arrEnd && filtersSize < CONFIG_MAX_RECORDS) {
        const char *objStart, *objEnd;
        char *namePtr = NULL, *filterPtr = NULL;
        const char *q;

        while (p < arrEnd && *p != '{' && *p != ']') p++;
        if (p >= arrEnd || *p == ']') break;

        objStart = p + 1;
        depth = 1;
        p++;
        while (p < arrEnd && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        objEnd = p - 1; // points to '}'

        // parse "name" and "filter" from this object
        q = objStart;
        while (q < objEnd) {
            const char *keyStart, *keyEnd, *valStart, *valEnd;
            int isName, isFilter;

            while (q < objEnd && *q != '"') q++;
            if (q >= objEnd) break;
            q++; // skip opening quote
            keyStart = q;
            while (q < objEnd && *q != '"') q++;
            if (q >= objEnd) break;
            keyEnd = q;
            q++; // skip closing quote

            isName = (keyEnd - keyStart == 4 && memcmp(keyStart, "name", 4) == 0);
            isFilter = (keyEnd - keyStart == 6 && memcmp(keyStart, "filter", 6) == 0);

            // skip to value (colon + whitespace + opening quote)
            while (q < objEnd && *q != '"') q++;
            if (q >= objEnd) break;
            q++; // skip opening quote
            valStart = q;
            while (q < objEnd && *q != '"') q++;
            if (q >= objEnd) break;
            valEnd = q;
            q++; // skip closing quote

            // null-terminate in-place (configBuf is mutable)
            if (isName) {
                namePtr = (char*)valStart;
                *(char*)valEnd = '\0';
            } else if (isFilter) {
                filterPtr = (char*)valStart;
                *(char*)valEnd = '\0';
            }
        }

        if (namePtr && filterPtr) {
            addPreset(namePtr, filterPtr);
        }
    }

    LOG("Loaded %d records from config.json.", filtersSize);
    return filtersSize > 0;
}

// parse legacy config.txt — "name: filter" per line, '#' comments
static int loadConfigTxt(const char *path) {
    FILE *f;
    size_t len;
    char *current, *last;

    f = fopen(path, "r");
    if (!f) return 0;

    len = fread(configBuf, sizeof(char), CONFIG_BUF_SIZE, f);
    fclose(f);
    if (len == CONFIG_BUF_SIZE) {
        LOG("Config file is larger than %d bytes, get truncated.", CONFIG_BUF_SIZE);
    }
    configBuf[len] = '\n';
    configBuf[len+1] = '\0';

    filtersSize = 0;
    last = current = configBuf;
    do {
        char *namePart;
EAT_SPACE:  while (isspace((unsigned char)*current)) { ++current; }
        if (*current == '#') {
            current = strchr(current, '\n');
            if (!current) break;
            current = current + 1;
            goto EAT_SPACE;
        }

        last = current;
        current = strchr(last, ':');
        if (!current) break;
        *current = '\0';
        namePart = last;
        current += 1;
        while (isspace((unsigned char)*current)) { ++current; }
        last = current;
        current = strchr(last, '\n');
        if (!current) break;
        *current = '\0';
        if (current > last && *(current-1) == '\r') *(current-1) = '\0';
        addPreset(namePart, last);
        last = current = current + 1;
    } while (last && last - configBuf < CONFIG_BUF_SIZE);

    LOG("Loaded %d records from config.txt.", filtersSize);
    return filtersSize > 0;
}

// loading up filters — try config.json first, fall back to config.txt
static void loadConfig() {
    char path[MSG_BUFSIZE];

    buildConfigPath(path, MSG_BUFSIZE, CONFIG_JSON_FILE);
    LOG("Trying config: %s", path);
    if (loadConfigJson(path)) return;

    buildConfigPath(path, MSG_BUFSIZE, CONFIG_TXT_FILE);
    LOG("Trying config: %s", path);
    if (loadConfigTxt(path)) return;

    INFO("No config.json/config.txt found. Using a single built-in preset.");
    filtersSize = 0;
    addPreset("loopback packets",
              "outbound and ip.DstAddr >= 127.0.0.1 and ip.DstAddr <= 127.255.255.255");
}

int         appPresetCount(void)         { return filtersSize; }
const char* appPresetName(int ix)        { return (ix >= 0 && ix < filtersSize) ? filters[ix].name : ""; }
const char* appPresetFilter(int ix)      { return (ix >= 0 && ix < filtersSize) ? filters[ix].filter : ""; }

// Escape a string for embedding into a JSON string literal.
static void jsonEscapeInto(const char *src, char *dst, int dstSize) {
    int i = 0;
    for (; *src && i < dstSize - 2; src++) {
        if (*src == '"' || *src == '\\') {
            dst[i++] = '\\';
            dst[i++] = *src;
        } else if ((unsigned char)*src < 0x20) {
            dst[i++] = ' ';
        } else {
            dst[i++] = *src;
        }
    }
    dst[i] = '\0';
}

int appPresetSave(const char *name, const char *filter) {
    char path[MSG_BUFSIZE];
    FILE *f;
    int i, existing = -1;

    if (!name || !name[0] || !filter || !filter[0]) return 0;

    for (i = 0; i < filtersSize; i++) {
        if (strcmp(filters[i].name, name) == 0) { existing = i; break; }
    }
    if (existing >= 0) {
        strncpy(filters[existing].filter, filter, FILTER_BUFSIZE - 1);
        filters[existing].filter[FILTER_BUFSIZE - 1] = '\0';
    } else {
        if (filtersSize >= CONFIG_MAX_RECORDS) return 0;
        addPreset(name, filter);
    }

    buildConfigPath(path, MSG_BUFSIZE, CONFIG_JSON_FILE);
    f = fopen(path, "w");
    if (!f) {
        INFO("presets: failed to write %s", path);
        return 0;
    }
    fprintf(f, "{\n  \"filters\": [\n");
    for (i = 0; i < filtersSize; i++) {
        char en[PRESET_NAME_SIZE * 2], ef[FILTER_BUFSIZE * 2];
        jsonEscapeInto(filters[i].name, en, sizeof(en));
        jsonEscapeInto(filters[i].filter, ef, sizeof(ef));
        fprintf(f, "    { \"name\": \"%s\", \"filter\": \"%s\" }%s\n",
                en, ef, (i < filtersSize - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    INFO("presets: saved '%s' to %s", name, path);
    return 1;
}

// ---------------------------------------------------------------------------
// Application state and control surface
// ---------------------------------------------------------------------------
static CRITICAL_SECTION appLock;
static volatile short   capturing     = 0;
static volatile short   quitRequested = 0;
static char             currentFilter[FILTER_BUFSIZE]  = "";
static char             currentProcess[MSG_BUFSIZE]    = "";
static char             statusLine[MSG_BUFSIZE]        = "Idle.";
static DWORD            captureStartTick = 0;

void showStatus(const char *line) {
    if (!line) return;
    strncpy(statusLine, line, MSG_BUFSIZE - 1);
    statusLine[MSG_BUFSIZE - 1] = '\0';
    INFO("%s", statusLine);
}

const char* appStatusLine(void)   { return statusLine; }
int         appIsCapturing(void)  { return capturing; }
const char* appCurrentFilter(void)  { return currentFilter; }
const char* appCurrentProcess(void) { return currentProcess; }
DWORD       appCaptureElapsedMs(void) {
    return capturing ? (GetTickCount() - captureStartTick) : 0;
}
void appRequestQuit(void) { InterlockedExchange16(&quitRequested, 1); }

// strncpy leaves the destination unterminated when the source exactly fills it,
// and several early returns below skip any later fix-up, so all error text goes
// through here.
static void setErr(char *errBuf, int errSize, const char *msg) {
    if (!errBuf || errSize <= 0) return;
    strncpy(errBuf, msg, errSize - 1);
    errBuf[errSize - 1] = '\0';
}

// Start capture. Safe to call from the HTTP or pipe thread - appLock serializes
// against the main tick loop, which is the only other writer of this state.
int appStartCapture(const char *filter, const char *procName,
                    char *errBuf, int errSize) {
    // User filter + optional process port fragment + the impostor clause below,
    // each with room to spare rather than a silent truncation at the boundary.
    char combinedFilter[FILTER_BUFSIZE * 2 + 64];
    char divertMsg[MSG_BUFSIZE];
    int ok = 0;

    if (!filter || !filter[0]) {
        setErr(errBuf, errSize, "filter expression is empty");
        return 0;
    }

    EnterCriticalSection(&appLock);

    if (capturing) {
        setErr(errBuf, errSize, "already capturing");
        LeaveCriticalSection(&appLock);
        return 0;
    }

    if (!IsRunAsAdmin()) {
#if defined(_WIN32)
        setErr(errBuf, errSize,
               "clumsy needs Administrator rights to open the WinDivert driver. "
               "Restart the console as Administrator.");
#else
        setErr(errBuf, errSize,
               "clumsy needs CAP_NET_ADMIN to open an NFQUEUE. Run it under sudo, "
               "or grant it once with: sudo setcap cap_net_admin,cap_net_raw+ep ./clumsy");
#endif
        LeaveCriticalSection(&appLock);
        return 0;
    }

    // build combined filter: user filter + optional process port filter
    {
        char procFragment[FILTER_BUFSIZE];
        const char *impostorClause = "";

        procFragment[0] = '\0';
        if (procName && procName[0] != '\0') {
            char errMsg[MSG_BUFSIZE];
            int result = buildProcessFilter(procName, procFragment, sizeof(procFragment),
                                            errMsg, sizeof(errMsg));
            if (result < 0) {
                setErr(errBuf, errSize, errMsg);
                LeaveCriticalSection(&appLock);
                return 0;
            }
            if (result > 0) INFO("Process filter: %s (%d ports)", procName, result);
            else            procFragment[0] = '\0';
        }

#if defined(_WIN32)
        // Replayed packets leave through a separate send-only WinDivert handle,
        // so - unlike the clones duplicate.cpp sends on the capture handle -
        // they come straight back in through this filter, get shaped a second
        // time, and are counted twice in /api/stats, --pcap-out and the HTML
        // report. Nobody wants clumsy to degrade its own output, so impostors
        // are excluded by default. Naming "impostor" in your own filter turns
        // this off, which is the way to capture another WinDivert tool's
        // injected traffic on purpose.
        if (!strstr(filter, "impostor")) impostorClause = " and not impostor";
#endif

        // The user filter is parenthesised before anything is appended: "and"
        // binds tighter than "or", so "outbound or inbound" plus a bare
        // "and not impostor" would silently become
        // "outbound or (inbound and not impostor)".
        snprintf(combinedFilter, sizeof(combinedFilter), "(%s)%s%s",
                 filter, procFragment, impostorClause);
    }

    // Refuse a filter another running clumsy could also match: whichever saw
    // a shared packet first would shape it and the other would never know.
    {
        const char *match = combinedFilter;
        int claim;
#if !defined(_WIN32)
        // With --auto-iptables the competing thing is the NFQUEUE rules, which
        // can be wider than the filter: a packet the first instance's rule
        // takes is accepted there and never reaches the second one's queue.
        char captureExpr[FILTER_BUFSIZE * 4];
        if (parseBoolValue(argGet("auto-iptables")) &&
            filterCaptureExpr(combinedFilter, captureExpr, sizeof(captureExpr))) {
            match = captureExpr;
        }
#endif
        claim = instanceClaim(match, filter, errBuf, errSize);
        if (claim != 1) {
            LeaveCriticalSection(&appLock);
            return claim;
        }
    }

    // Before divertStart(), not after: it spawns the read and clock loops
    // already running, and with lag or jitter left enabled from a previous run
    // the first packets can reach latencyRecord() while this thread is still
    // on its way to the reset - which would then throw those samples away.
    statsReset();

    divertMsg[0] = '\0';
    if (divertStart(combinedFilter, divertMsg)) {
        strncpy(currentFilter, filter, FILTER_BUFSIZE - 1);
        currentFilter[FILTER_BUFSIZE - 1] = '\0';
        strncpy(currentProcess, procName ? procName : "", MSG_BUFSIZE - 1);
        currentProcess[MSG_BUFSIZE - 1] = '\0';
        captureStartTick = GetTickCount();
        InterlockedExchange16(&capturing, 1);
        ok = 1;
    } else {
        setErr(errBuf, errSize, divertMsg);
        instanceRelease();
    }

    if (ok) {
        // Started under appLock, exactly as appStopCapture() stops them.
        // statslog.cpp and pcapexport.cpp are only as thread-safe as their
        // callers make them, so opening a log file outside the lock would let
        // a stop request arriving on another HTTP worker close the FILE* this
        // thread is still writing to - or miss it entirely and leak it.
        reportSessionStart(combinedFilter);

        // start stats log if --stats-log was given
        {
            const char *logPath = argGet("stats-log");
            if (logPath) statsLogStart(logPath, argGetInt("stats-interval", 1));
        }
        // start pcap capture if --pcap-out was given
        {
            const char *pcapPath = argGet("pcap-out");
            if (pcapPath && !pcapExportIsActive()) {
                pcapExportStart(pcapPath,
                                (long)argGetInt("pcap-max-packets", 0),
                                (long)argGetInt("pcap-max-bytes", 0));
            }
        }
        scenarioStart();
    }

    LeaveCriticalSection(&appLock);

    if (ok) {
        char buf[MSG_BUFSIZE];
        // The filter can be up to FILTER_BUFSIZE; cap what goes into the
        // MSG_BUFSIZE status line rather than letting snprintf truncate blindly.
        snprintf(buf, MSG_BUFSIZE, "Capturing. filter=\"%.400s\"%s%.64s", filter,
                 currentProcess[0] ? " process=" : "", currentProcess);
        showStatus(buf);
    }
    return ok;
}

void appStopCapture(void) {
    int wasCapturing;

    EnterCriticalSection(&appLock);
    wasCapturing = capturing;
    if (wasCapturing) {
        int ix;
        scenarioStop();
        divertStop();
        InterlockedExchange16(&capturing, 0);
        for (ix = 0; ix < MODULE_CNT; ++ix) {
            modules[ix]->processTriggered = 0; // threads already stopped
        }
        sendState = SEND_STATUS_NONE;
        statsLogStop();
        pcapExportStop();
        reportSessionStop();
        instanceRelease();
    }
    LeaveCriticalSection(&appLock);

    if (wasCapturing) {
        const char *reportPath = argGet("report-out");
        if (reportPath) reportWriteHtml(reportPath);
        showStatus("Stopped. Set a filter and start again to resume.");
        statsReset();
    }
}

// ---------------------------------------------------------------------------
// Startup helpers
// ---------------------------------------------------------------------------

// Apply every --<module> / --<module>-<param> argument to its module.
static void applyCliModuleParams(void) {
    int ix, p;
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        Module *m = modules[ix];
        const char *v = argGet(m->shortName);
        if (v) {
            InterlockedExchange16(m->enabledFlag, (short)parseBoolValue(v));
            INFO("module %s: %s", m->shortName, *m->enabledFlag ? "enabled" : "disabled");
        }
        if (!m->setParam) continue;
        for (p = 0; p < m->paramSpecCount; ++p) {
            const char *key = m->paramSpecs[p].key;
            const char *pv = argGet(key);
            if (pv) {
                char now[PARAM_VAL_SIZE];
                m->setParam(key, pv);
                // What the module is running with, not what was typed: the two
                // differ whenever setParam clamped, and echoing the request
                // back would report a value that is not in effect.
                INFO("param  %s = %s", key,
                     moduleParamCurrent(m, key, now, sizeof(now)) ? now : pv);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Command line validation
//
// parseArgs() accepts any "--key value" pair, and every reader of the argument
// store asks for the keys it knows, so a misspelt option used to be dropped
// without a word: "--lag-ms 100" started clumsy with lag at its default and an
// exit code of 0. That is survivable for someone watching the console and
// fatal for a script or a language model, which can only judge by the result.
// So the command line is checked as a whole before anything starts.
// ---------------------------------------------------------------------------

// Longest key the argument store keeps (ARG_KEY_SIZE in utils.cpp).
#define ARG_KEY_MAX 48

// Process exit codes. Documented in --help, README and llms.txt.
#define EXIT_BAD_USAGE       2   // unknown option, unusable value, missing input file
#define EXIT_START_FAILED    3   // --filter / --replay-in could not start
#define EXIT_CONFLICT        4   // web port, pipe name or filter clashes with another clumsy

// Every non-module option some argGet() call reads. Add new options here as
// well as in printUsage(), or validateCliArgs() will reject them.
static const char *const globalArgs[] = {
    "filter", "process", "timeout", "elevate",
    "queue-num", "inject-mark", "auto-iptables",
    "web", "web-port", "web-bind", "web-token", "pipe-name",
    "profile", "scenario",
    "stats-log", "stats-interval", "stats-console",
    "pcap-out", "pcap-stage", "pcap-max-packets", "pcap-max-bytes",
    "replay-in", "replay-speed", "replay-loop",
    "report-out", "enable-plugins", "verbose",
};
#define GLOBAL_ARG_CNT ((int)(sizeof(globalArgs) / sizeof(globalArgs[0])))

// Names other tools use for the same thing. A model that has never seen clumsy
// reaches for these first, and "unknown option" alone does not get it from
// "--delay" to a module that has to be switched on separately.
static const struct { const char *wrong; const char *hint; } argSynonyms[] = {
    { "delay",       "--lag on --lag-time <ms>" },
    { "latency",     "--lag on --lag-time <ms>" },
    { "ping",        "--lag on --lag-time <ms>" },
    { "loss",        "--drop on --drop-chance <percent>" },
    { "packet-loss", "--drop on --drop-chance <percent>" },
    { "rate",        "--bandwidth on --bandwidth-bandwidth <KB/s>" },
    { "limit",       "--bandwidth on --bandwidth-bandwidth <KB/s>" },
    { "reorder",     "--ood on --ood-chance <percent>" },
    { "duration",    "--timeout <sec>" },
    { "time",        "--timeout <sec>" },
    { "port",        "a port test inside --filter, e.g. \"udp.DstPort == 7777\" "
                     "(--web-port only moves the control API)" },
    { "pid",         "--process <name.exe>" },
    { "output",      "--stats-log, --pcap-out or --report-out" },
    { "config",      "--profile <name> or --scenario <path>" },
};

static int editDistance(const char *a, const char *b) {
    int row[ARG_KEY_MAX + 1];
    int la = (int)strlen(a), lb = (int)strlen(b), i, j;

    if (la > ARG_KEY_MAX || lb > ARG_KEY_MAX) return ARG_KEY_MAX;
    for (j = 0; j <= lb; ++j) row[j] = j;
    for (i = 1; i <= la; ++i) {
        int diag = row[0];
        row[0] = i;
        for (j = 1; j <= lb; ++j) {
            int above = row[j];
            int cost = diag + (a[i - 1] == b[j - 1] ? 0 : 1);
            if (row[j - 1] + 1 < cost) cost = row[j - 1] + 1;
            if (above + 1 < cost)      cost = above + 1;
            row[j] = cost;
            diag = above;
        }
    }
    return row[lb];
}

// Writes the most useful thing to say about an option nothing recognises.
static void suggestForUnknownArg(const char *key, char *buf, int size) {
    const char *best = NULL;
    int bestDist = ARG_KEY_MAX, ix, p, pos = 0, limit;
    size_t i;

    buf[0] = '\0';

    for (i = 0; i < sizeof(argSynonyms) / sizeof(argSynonyms[0]); ++i) {
        if (_stricmp(key, argSynonyms[i].wrong) == 0) {
            snprintf(buf, (size_t)size, "clumsy spells that %s", argSynonyms[i].hint);
            return;
        }
    }

    // "--lag-ms": right module, wrong parameter. List what that module takes.
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        const Module *m = modules[ix];
        size_t nameLen = strlen(m->shortName);
        if (strncmp(key, m->shortName, nameLen) == 0 && key[nameLen] == '-') {
            pos = appendf(buf, size, pos, "%s takes: --%s on|off", m->shortName, m->shortName);
            for (p = 0; p < m->paramSpecCount; ++p) {
                pos = appendf(buf, size, pos, ", --%s", m->paramSpecs[p].key);
            }
            return;
        }
    }

    // Otherwise the nearest spelling, if one is near enough to be a typo.
    for (ix = 0; ix < GLOBAL_ARG_CNT; ++ix) {
        int d = editDistance(key, globalArgs[ix]);
        if (d < bestDist) { bestDist = d; best = globalArgs[ix]; }
    }
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        int d = editDistance(key, modules[ix]->shortName);
        if (d < bestDist) { bestDist = d; best = modules[ix]->shortName; }
        for (p = 0; p < modules[ix]->paramSpecCount; ++p) {
            d = editDistance(key, modules[ix]->paramSpecs[p].key);
            if (d < bestDist) { bestDist = d; best = modules[ix]->paramSpecs[p].key; }
        }
    }
    limit = (int)strlen(key) / 3;
    if (limit < 2) limit = 2;
    if (best && bestDist <= limit) {
        snprintf(buf, (size_t)size, "did you mean --%s?", best);
    }
}

// Checks every stored argument. Prints one line per problem to stderr and
// returns how many make the command line unusable. Out-of-range numbers are
// not among them: they are clamped as they always were, with a warning, so
// existing scripts keep running.
static int validateCliArgs(void) {
    int errors = 0, i, ix;

    for (i = 0; i < argStoredCount(); ++i) {
        const char *key = argStoredKey(i);
        const char *value = argGet(key);
        const ParamSpec *spec;
        char msg[MSG_BUFSIZE];
        int known = 0;

        for (ix = 0; ix < GLOBAL_ARG_CNT && !known; ++ix) {
            if (strcmp(key, globalArgs[ix]) == 0) known = 1;
        }
        if (known) continue;

        for (ix = 0; ix < MODULE_CNT && !known; ++ix) {
            if (strcmp(key, modules[ix]->shortName) == 0) known = 1;
        }
        if (known) {
            if (!isBoolWord(value)) {
                fprintf(stderr, "clumsy: --%s %s: use on or off\n", key, value);
                errors++;
            }
            continue;
        }

        spec = paramSpecFind(key, NULL);
        if (spec) {
            int verdict = paramCheckValue(spec, value, msg, sizeof(msg));
            if (verdict == PARAM_INVALID) {
                fprintf(stderr, "clumsy: --%s\n", msg);
                errors++;
            } else if (verdict == PARAM_ADJUSTED) {
                fprintf(stderr, "clumsy: warning: --%s\n", msg);
            }
            continue;
        }

        suggestForUnknownArg(key, msg, sizeof(msg));
        fprintf(stderr, "clumsy: unknown option --%s%s%s\n", key, msg[0] ? " - " : "", msg);
        errors++;
    }
    return errors;
}

#if defined(_WIN32)
static BOOL WINAPI consoleCtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        INFO("\nShutting down...");
        appRequestQuit();
        return TRUE;
    default:
        return FALSE;
    }
}
#else
static void posixSignalHandler(int sig) {
    UNREFERENCED_PARAMETER(sig);
    // Only async-signal-safe work here: flip the flag, let the tick loop unwind.
    appRequestQuit();
}
#endif

// Arranges for Ctrl+C / SIGTERM to request a clean shutdown.
static void installShutdownHandler(void) {
#if defined(_WIN32)
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    signal(SIGINT,  posixSignalHandler);
    signal(SIGTERM, posixSignalHandler);
    signal(SIGHUP,  posixSignalHandler);
#endif
}

// The module half of --help, generated from the same ParamSpec tables the web
// form and the REST API are built from. A hand-written list here went stale
// the day a module was added, and "--<module>-<param> <v>" on its own leaves
// the reader to guess that the delay is --lag-time and not --lag-ms.
static void printModuleUsage(void) {
    int ix, p;

    printf("Modules (%d), applied to each packet in this order. All start off.\n",
           MODULE_CNT);
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        const Module *m = modules[ix];
        char line[96];

        snprintf(line, sizeof(line), "--%s on|off", m->shortName);
        printf("  %-22s %s\n", line, moduleDescription(m->shortName)[0]
                                     ? moduleDescription(m->shortName) : m->displayName);

        for (p = 0; p < m->paramSpecCount; ++p) {
            const ParamSpec *s = &m->paramSpecs[p];
            const char *tail = s->key + strlen(s->key);
            char now[PARAM_VAL_SIZE] = "";
            int isBool = strcmp(s->type, "bool") == 0;

            // Direction switches are the same two on every module; they are
            // described once below rather than 24 times here.
            if (tail - s->key > 8 && strcmp(tail - 8, "-inbound") == 0)  continue;
            if (tail - s->key > 9 && strcmp(tail - 9, "-outbound") == 0) continue;

            if (isBool) {
                snprintf(line, sizeof(line), "--%s on|off", s->key);
            } else if (strcmp(s->type, "action") == 0) {
                snprintf(line, sizeof(line), "--%s on", s->key);
            } else if (strcmp(s->type, "enum") == 0) {
                snprintf(line, sizeof(line), "--%s <%s>", s->key, s->options ? s->options : "");
                for (char *c = line; *c; ++c) if (*c == ',') *c = '|';
            } else if (s->minVal != s->maxVal) {
                snprintf(line, sizeof(line), "--%s <%.10g..%.10g>", s->key, s->minVal, s->maxVal);
            } else {
                snprintf(line, sizeof(line), "--%s <%s>", s->key, s->type);
            }

            if (strcmp(s->type, "action") == 0) {
                printf("    %-32s %s (one-shot action)\n", line, s->label);
            } else if (moduleParamCurrent(m, s->key, now, sizeof(now))) {
                // --help runs before any argument is applied, so what the
                // module reports here is its default.
                const char *shown = now;
                if (isBool) shown = parseBoolValue(now) ? "on" : "off";
                printf("    %-32s %s, default %s\n", line, s->label, shown);
            } else {
                printf("    %-32s %s\n", line, s->label);
            }
        }
    }
    printf(
"  Every module also takes --<module>-inbound on|off and\n"
"  --<module>-outbound on|off (both default on) to pick a direction.\n"
"  Chances are percentages, not fractions: --drop-chance 5 means 5%%.\n");
}

static void printUsage(void) {
    printf(
"clumsy " CLUMSY_VERSION " - console + web network condition simulator\n"
"\n"
"Usage: clumsy [--key value]...\n"
"\n"
"Capture:\n"
"  --filter \"<expr>\"        WinDivert filter; capture starts immediately\n"
"                           e.g. both directions of UDP port 7777:\n"
"                             \"udp and (udp.DstPort == 7777 or udp.SrcPort == 7777)\"\n"
"                           one remote host:\n"
"                             \"ip.DstAddr == 10.0.0.5 or ip.SrcAddr == 10.0.0.5\"\n"
"                           localhost traffic:\n"
"                             \"outbound and loopback\"\n"
"                           Without it clumsy idles until a filter arrives\n"
"                           over the dashboard or POST /api/filter.\n"
"  --process <name.exe>     restrict capture to one process's local ports\n"
"  --timeout <sec>          quit automatically after N seconds\n"
#if defined(_WIN32)
"  --elevate on             relaunch elevated when not already Administrator\n"
#else
"  --queue-num <n>          NFQUEUE queue number to bind (default 0)\n"
"  --inject-mark <n>        fwmark stamped on injected packets (default 0xC1);\n"
"                           ACCEPT it before your NFQUEUE rule\n"
"  --auto-iptables on       let clumsy install and remove the NFQUEUE rules\n"
"                           itself, derived from the filter expression\n"
"  --elevate on             print how to obtain CAP_NET_ADMIN\n"
#endif
"\n"
"Web dashboard:\n"
"  --web off                disable the HTTP server\n"
"  --web-port <n>           listen port (default 8080)\n"
"  --web-bind <addr>        bind address (default 127.0.0.1; token required\n"
"                           for anything else)\n"
"  --web-token <token>      fixed auth token instead of a random one\n"
"\n"
"Several instances (e.g. one per client, each with its own conditions):\n"
#if defined(_WIN32)
"  --pipe-name <name>       control pipe \\\\.\\pipe\\<name> (default clumsy)\n"
#else
"  --pipe-name <name>       control socket /run/<name>.sock (default clumsy)\n"
#endif
"  Give every instance its own --web-port and --pipe-name"
#if !defined(_WIN32)
" (and --queue-num)"
#endif
". A filter that\n"
"  could match the same packets as another running instance is refused,\n"
"  naming that instance and an example packet. To give one client per\n"
"  instance, pin both ports of the client's flow:\n"
"    \"(udp.SrcPort == 50001 and udp.DstPort == 7777) or\n"
"     (udp.SrcPort == 7777 and udp.DstPort == 50001)\"\n"
"\n");

    printModuleUsage();

    printf(
"\n"
"Automation:\n"
"  --profile <name>         apply a profile from profiles.json\n"
"  --scenario <path>        load a scenario json and play it on start\n"
"  --stats-log <path>       write periodic stats (.csv or .json)\n"
"  --stats-interval <sec>   stats log interval (default 1)\n"
"  --stats-console <sec>    console heartbeat interval (default 10, 0=off)\n"
"  --pcap-out <path>        dump captured packets to a libpcap file\n"
"  --pcap-stage <s>         pre | post | both: before or after the modules\n"
"                           ran (default post)\n"
"  --pcap-max-packets <n>   stop the pcap dump after N packets (0=unlimited)\n"
"  --pcap-max-bytes <n>     stop the pcap dump after N bytes  (0=unlimited)\n"
"  --replay-in <path>       replay a libpcap file back onto the wire\n"
"  --replay-speed <x>       replay rate multiplier (default 1.0)\n"
"  --replay-loop on         restart the replay when the file ends\n"
"  --report-out <path>      write an HTML session report on stop\n"
"  --enable-plugins <dir>   load custom module DLLs from <dir>\n"
"  --verbose on|off         per-packet trace logging\n"
"  --help                   show this text\n"
"  --version                print the version\n"
"\n"
"Scripts and AI agents:\n"
"  * Every option is a \"--key value\" pair; switches take on or off.\n"
"  * An unknown option or an unusable value stops clumsy before it starts\n"
"    (exit 2). An out-of-range number is clamped and reported.\n"
"  * clumsy runs until Ctrl+C, --timeout, or POST /api/quit. Start it in the\n"
"    background, poll GET /api/health, and always pass --timeout as a\n"
"    safety net so a forgotten instance cannot keep degrading the network.\n"
"  * While it runs, GET http://127.0.0.1:8080/api/docs lists every REST\n"
"    endpoint and GET /llms.txt is a plain-text guide written for agents.\n"
"  * Capture needs "
#if defined(_WIN32)
"an Administrator console"
#else
"CAP_NET_ADMIN"
#endif
"; GET /api/status reports \"admin\".\n"
"\n"
"Exit codes:\n"
"  0  clean exit (Ctrl+C, --timeout, POST /api/quit)\n"
"  2  bad command line: unknown option, unusable value, or a --profile /\n"
"     --scenario that could not be loaded\n"
"  3  the capture (--filter) or replay (--replay-in) could not start\n"
"  4  clash with another running clumsy: its --web-port or --pipe-name is\n"
"     taken, or the filter overlaps another instance's capture\n");
}

int main(int argc, char* argv[]) {
    DWORD lastTick, statsConsoleTick = 0;
    int   timeoutSec, statsConsoleSec;
    DWORD startTick;
    int   webEnabled;
    int   exitCode = 0;

    InitializeCriticalSection(&appLock);
    pcapExportInit();
    pcapReplayInit();
    reportInit();
    latencyInit();
    profileInit();
    srand((unsigned int)time(NULL));

    if (argc > 1) {
        char argErr[MSG_BUFSIZE];
        int i;

        // Anywhere on the line, not only first: "clumsy --filter udp --help"
        // is someone asking for help, and --help takes no value so parseArgs
        // would otherwise reject the whole line over it.
        for (i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "/?") == 0) {
                printUsage();
                return 0;
            }
            if (strcmp(argv[i], "--version") == 0) {
                printf("clumsy " CLUMSY_VERSION "\n");
                return 0;
            }
        }
        if (!parseArgs(argc, argv, argErr, sizeof(argErr))) {
            fprintf(stderr, "clumsy: %s\nRun clumsy --help for every option.\n", argErr);
            return EXIT_BAD_USAGE;
        }
        if (validateCliArgs() > 0) {
            fprintf(stderr, "Nothing was started. Run clumsy --help for every option.\n");
            return EXIT_BAD_USAGE;
        }
        parameterized = TRUE;
    }

    {
        const char *v = argGet("verbose");
        if (v) InterlockedExchange16(&logVerbose, (short)parseBoolValue(v));
    }

    installShutdownHandler();

    if (argGet("pipe-name") && !pipeNameValid(argGet("pipe-name"))) {
        fprintf(stderr, "clumsy: --pipe-name %s: use 1-64 letters, digits, '-', '_' or '.'\n"
                        "Nothing was started. Run clumsy --help for every option.\n",
                argGet("pipe-name"));
        return EXIT_BAD_USAGE;
    }

    // Elevation is opt-in now: a console app that silently relaunches itself in
    // a new window is more confusing than a clear message. The web UI still
    // comes up without admin so the operator can see what is wrong (Phase 2.10).
    if (!IsRunAsAdmin()) {
        if (parseBoolValue(argGet("elevate"))) {
            if (tryElevate(FALSE)) return 0; // an elevated instance took over
        }
#if defined(_WIN32)
        INFO("WARNING: not running as Administrator.");
        INFO("         The dashboard will start, but capture cannot open the "
             "WinDivert driver.");
        INFO("         Restart this console as Administrator, or pass --elevate on.");
#else
        INFO("WARNING: missing CAP_NET_ADMIN.");
        INFO("         The dashboard will start, but capture cannot open an NFQUEUE.");
        tryElevate(FALSE);   // prints the sudo / setcap guidance
#endif
    }

    loadConfig();
    profilesLoad();
    applyCliModuleParams();

    if (argGet("enable-plugins")) {
        pluginLoadDir(argGet("enable-plugins"));
    }

    {
        const char *scenarioPath = argGet("scenario");
        if (scenarioPath) {
            char lint[2048];

            scenarioLoad(scenarioPath);
            INFO("scenario: %s (%s, %d steps)", scenarioPath,
                 scenarioIsLoaded() ? "loaded" : "failed to load", scenarioStepCount());
            // A run that was asked to play a scenario and has none is not the
            // run that was asked for; carrying on would report success for a
            // test that never applied a single condition.
            if (!scenarioIsLoaded()) {
                fprintf(stderr, "clumsy: --scenario %s has no usable steps. It must be a JSON "
                                "array of objects, each with an \"at\" or a \"when\" trigger.\n",
                        scenarioPath);
                return EXIT_BAD_USAGE;
            }
            if (scenarioLint(lint, sizeof(lint)) > 0) {
                char *line = lint;
                while (line && *line) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';
                    INFO("scenario: WARNING %s", line);
                    line = nl ? nl + 1 : NULL;
                }
            }
        }
    }

    {
        const char *profileName = argGet("profile");
        if (profileName) {
            int applied = profileApply(profileName);
            INFO("profile '%s': %s", profileName, applied ? "applied" : "not found");
            if (!applied) {
                char names[MAX_PROFILES][PROFILE_NAME_SIZE];
                int cnt = profileNamesSnapshot(names, MAX_PROFILES), i;

                fprintf(stderr, "clumsy: --profile %s is not in profiles.json. ", profileName);
                if (cnt == 0) {
                    fprintf(stderr, "No profiles are saved yet; create one with "
                                    "POST /api/profiles or the dashboard, or pass the "
                                    "module options directly.\n");
                } else {
                    fprintf(stderr, "Saved profiles:");
                    for (i = 0; i < cnt; ++i) fprintf(stderr, " %s", names[i]);
                    fprintf(stderr, "\n");
                }
                return EXIT_BAD_USAGE;
            }
        }
    }

    // A pipe name or web port that another clumsy holds is fatal, not a
    // warning: a script talking to "port 8080" or "\\.\pipe\clumsy" would
    // silently be driving that other instance instead of this one.
    if (pipeServerStart() < 0) {
        fprintf(stderr, "clumsy: the control pipe name is in use by another clumsy. Pass a "
                        "unique --pipe-name (and --web-port) to run several at once.\n");
        return EXIT_CONFLICT;
    }

    webEnabled = 1;
    if (argGet("web") && !parseBoolValue(argGet("web"))) webEnabled = 0;
    if (webEnabled) {
        const char *bindAddr = argGet("web-bind");
        int port = argGetInt("web-port", 8080);
        const int started = httpServerStart(bindAddr ? bindAddr : "127.0.0.1", port,
                                            argGet("web-token"));
        if (started < 0) {
            fprintf(stderr, "clumsy: web port %d is in use, by another clumsy or another "
                            "program. Pass a free --web-port (and a unique --pipe-name) to "
                            "run several at once.\n", port);
            pipeServerStop();
            return EXIT_CONFLICT;
        }
        if (!started) {
            INFO("WARNING: web dashboard failed to start; continuing without it.");
            webEnabled = 0;
        }
    }

    // ---- banner ----
    INFO("");
    INFO("clumsy " CLUMSY_VERSION " - console + web network condition simulator");
#if defined(_WIN32)
    INFO("  Administrator : %s", IsRunAsAdmin() ? "yes" : "NO (capture disabled)");
#else
    INFO("  CAP_NET_ADMIN : %s", IsRunAsAdmin() ? "yes" : "NO (capture disabled)");
#endif
    if (webEnabled) {
        INFO("  Web dashboard : %s", httpServerUrl());
    } else {
        INFO("  Web dashboard : disabled");
    }
#if defined(_WIN32)
    INFO("  Named Pipe    : %s", pipeServerPath()[0] ? pipeServerPath() : "unavailable");
#else
    INFO("  Control socket: %s",
         pipeServerPath()[0] ? pipeServerPath() : "unavailable");
    INFO("  NFQUEUE num   : %d  (--queue-num to change)", argGetInt("queue-num", 0));
#endif
    INFO("  Presets       : %d loaded", appPresetCount());
    INFO("  Profiles      : %d loaded", profileCount());
    INFO("  Press Ctrl+C to quit.");
    INFO("");

    // auto-start when a filter was given on the command line
    if (argGet("filter")) {
        char err[1024] = "";
        const int started = appStartCapture(argGet("filter"), argGet("process"), err, sizeof(err));
        if (started <= 0) {
            // --filter is a request to capture now. Idling with the dashboard
            // up is right when no filter was given, but here it would leave a
            // script waiting out its whole --timeout on a clumsy that shapes
            // nothing and then exits 0. Fail the run instead.
            INFO("Failed to start capture: %s", err);
            exitCode = (started == CAPTURE_CONFLICT) ? EXIT_CONFLICT : EXIT_START_FAILED;
        }
    }

    // Replay is independent of capture: it can feed a recorded session back
    // onto the wire with no filter set at all.
    if (argGet("replay-in") && exitCode == 0) {
        char err[MSG_BUFSIZE] = "";
        const char *speedArg = argGet("replay-speed");
        double speed = speedArg ? atof(speedArg) : 1.0;
        if (!pcapReplayStart(argGet("replay-in"), speed,
                             parseBoolValue(argGet("replay-loop")),
                             err, sizeof(err))) {
            INFO("Failed to start replay: %s", err);
            exitCode = EXIT_START_FAILED;
        }
    }

    if (exitCode != 0) {
        INFO("Exiting with code %d; nothing is being shaped.", exitCode);
        appRequestQuit();   // skips the tick loop and runs the normal cleanup
    }

    timeoutSec      = argGetInt("timeout", 0);
    statsConsoleSec = argGetInt("stats-console", 10);
    startTick       = GetTickCount();
    lastTick        = startTick;
    statsConsoleTick = startTick;

    // ---- main tick loop ----
    while (!quitRequested) {
        DWORD now;

        Sleep(CLOCK_TICK_MS);
        now = GetTickCount();

        if (pipeStopRequested) {
            INFO("stop requested over the control API.");
            break;
        }

        EnterCriticalSection(&appLock);
        if (capturing) {
            scenarioTick();
            statsLogTick();
            reportTick();
        }
        LeaveCriticalSection(&appLock);

        if (capturing && statsConsoleSec > 0 &&
            (now - statsConsoleTick) >= (DWORD)(statsConsoleSec * 1000)) {
            int ix, pos = 0;
            char mbuf[512];
            mbuf[0] = '\0';
            for (ix = 0; ix < MODULE_CNT; ++ix) {
                LONG cnt = modules[ix]->affectedCount;
                if ((cnt > 0 || *(modules[ix]->enabledFlag)) &&
                    pos < (int)sizeof(mbuf) - 32) {
                    pos += snprintf(mbuf + pos, sizeof(mbuf) - pos, " %s=%ld",
                                    modules[ix]->shortName, cnt);
                }
            }
            INFO("[%4lus] captured=%ld sent=%ld%s",
                 (unsigned long)((now - captureStartTick) / 1000),
                 statsCapturedTotal, statsSentTotal, mbuf);
            statsConsoleTick = now;
        }

        if (timeoutSec > 0 && (now - startTick) >= (DWORD)(timeoutSec * 1000)) {
            INFO("timeout of %ds reached.", timeoutSec);
            break;
        }
        lastTick = now;
    }
    (void)lastTick;

    // ---- cleanup ----
    pcapReplayStop();   // joins the replay thread before the backend closes
    appStopCapture();
    httpServerStop();
    pipeServerStop();
    pluginUnloadAll();
    endTimePeriod();
    DeleteCriticalSection(&appLock);
    INFO("clumsy exited.");
    return exitCode;
}
