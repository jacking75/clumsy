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
    char combinedFilter[FILTER_BUFSIZE * 2];
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
    if (procName && procName[0] != '\0') {
        char procFragment[FILTER_BUFSIZE];
        char errMsg[MSG_BUFSIZE];
        int result = buildProcessFilter(procName, procFragment, sizeof(procFragment),
                                        errMsg, sizeof(errMsg));
        if (result < 0) {
            setErr(errBuf, errSize, errMsg);
            LeaveCriticalSection(&appLock);
            return 0;
        }
        if (result > 0) {
            snprintf(combinedFilter, sizeof(combinedFilter), "(%s)%s", filter, procFragment);
            INFO("Process filter: %s (%d ports)", procName, result);
        } else {
            snprintf(combinedFilter, sizeof(combinedFilter), "%s", filter);
        }
    } else {
        snprintf(combinedFilter, sizeof(combinedFilter), "%s", filter);
    }

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
    }

    LeaveCriticalSection(&appLock);

    if (ok) {
        char buf[MSG_BUFSIZE];
        statsReset();
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

// Returns TRUE when another clumsy already holds the single-instance claim.
static BOOL checkIsRunning() {
#if defined(_WIN32)
    // It will be closed and destroyed when program terminates (according to MSDN).
    HANDLE hStartEvent = CreateEventW(NULL, FALSE, FALSE, L"Global\\CLUMSY_IS_RUNNING_EVENT_NAME");

    if (hStartEvent == NULL)
        return TRUE;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hStartEvent);
        return TRUE;
    }

    return FALSE;
#else
    // An advisory lock on a pidfile: the kernel drops it when the process exits,
    // however it exits, so a crashed clumsy never leaves a stale claim behind.
    // Deliberately left open for the lifetime of the process.
    static int lockFd = -1;
    lockFd = open("/var/lock/clumsy.lock", O_CREAT | O_RDWR, 0644);
    if (lockFd < 0) {
        // /var/lock may be unwritable (unprivileged run); fall back to /tmp.
        lockFd = open("/tmp/clumsy.lock", O_CREAT | O_RDWR, 0644);
    }
    if (lockFd < 0) return FALSE;   // cannot tell, so do not block startup

    if (flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
        close(lockFd);
        return TRUE;
    }
    return FALSE;
#endif
}

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
            const char *pv = argGet(m->paramSpecs[p].key);
            if (pv) {
                m->setParam(m->paramSpecs[p].key, pv);
                INFO("param  %s = %s", m->paramSpecs[p].key, pv);
            }
        }
    }
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

static void printUsage(void) {
    printf(
"clumsy " CLUMSY_VERSION " - console + web network condition simulator\n"
"\n"
"Usage: clumsy [--key value]...\n"
"\n"
"Capture:\n"
"  --filter \"<expr>\"        WinDivert filter; capture starts immediately\n"
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
"Modules (12):  lag jitter drop burstloss blackout throttle duplicate ood\n"
"               tamper corrupt reset bandwidth\n"
"  --<module> on|off        enable a module,   e.g. --drop on\n"
"  --<module>-<param> <v>   set a parameter,   e.g. --drop-chance 10.0\n"
"\n"
"Automation:\n"
"  --profile <name>         apply a profile from profiles.json\n"
"  --scenario <path>        load a scenario json and play it on start\n"
"  --stats-log <path>       write periodic stats (.csv or .json)\n"
"  --stats-interval <sec>   stats log interval (default 1)\n"
"  --stats-console <sec>    console heartbeat interval (default 10, 0=off)\n"
"  --pcap-out <path>        dump captured packets to a libpcap file\n"
"  --pcap-max-packets <n>   stop the pcap dump after N packets (0=unlimited)\n"
"  --pcap-max-bytes <n>     stop the pcap dump after N bytes  (0=unlimited)\n"
"  --replay-in <path>       replay a libpcap file back onto the wire\n"
"  --replay-speed <x>       replay rate multiplier (default 1.0)\n"
"  --replay-loop on         restart the replay when the file ends\n"
"  --report-out <path>      write an HTML session report on stop\n"
"  --enable-plugins <dir>   load custom module DLLs from <dir>\n"
"  --verbose on|off         per-packet trace logging\n"
"  --help                   show this text\n");
}

int main(int argc, char* argv[]) {
    DWORD lastTick, statsConsoleTick = 0;
    int   timeoutSec, statsConsoleSec;
    DWORD startTick;
    int   webEnabled;

    InitializeCriticalSection(&appLock);
    pcapExportInit();
    pcapReplayInit();
    reportInit();
    srand((unsigned int)time(NULL));

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
            strcmp(argv[1], "/?") == 0) {
            printUsage();
            return 0;
        }
        if (!parseArgs(argc, argv)) {
            fprintf(stderr, "Invalid arguments. Options are \"--key value\" pairs, "
                            "e.g. --drop on. Run with --help for the full list.\n");
            return -1;
        }
        parameterized = TRUE;
    }

    {
        const char *v = argGet("verbose");
        if (v) InterlockedExchange16(&logVerbose, (short)parseBoolValue(v));
    }

    installShutdownHandler();

    if (checkIsRunning()) {
        fprintf(stderr, "There's already an instance of clumsy running. Aborting.\n");
        return -1;
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
            scenarioLoad(scenarioPath);
            INFO("scenario: %s (%s, %d steps)", scenarioPath,
                 scenarioIsLoaded() ? "loaded" : "failed to load", scenarioStepCount());
        }
    }

    {
        const char *profileName = argGet("profile");
        if (profileName) {
            INFO("profile '%s': %s", profileName,
                 profileApply(profileName) ? "applied" : "not found");
        }
    }

    pipeServerStart();

    webEnabled = 1;
    if (argGet("web") && !parseBoolValue(argGet("web"))) webEnabled = 0;
    if (webEnabled) {
        const char *bindAddr = argGet("web-bind");
        int port = argGetInt("web-port", 8080);
        if (!httpServerStart(bindAddr ? bindAddr : "127.0.0.1", port, argGet("web-token"))) {
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
    INFO("  Named Pipe    : \\\\.\\pipe\\clumsy");
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
        char err[MSG_BUFSIZE] = "";
        if (!appStartCapture(argGet("filter"), argGet("process"), err, sizeof(err))) {
            INFO("Failed to start capture: %s", err);
        }
    }

    // Replay is independent of capture: it can feed a recorded session back
    // onto the wire with no filter set at all.
    if (argGet("replay-in")) {
        char err[MSG_BUFSIZE] = "";
        const char *speedArg = argGet("replay-speed");
        double speed = speedArg ? atof(speedArg) : 1.0;
        if (!pcapReplayStart(argGet("replay-in"), speed,
                             parseBoolValue(argGet("replay-loop")),
                             err, sizeof(err))) {
            INFO("Failed to start replay: %s", err);
        }
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
    return 0;
}
