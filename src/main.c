#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"

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
    &resetModule,
	&bandwidthModule,
};

volatile short sendState = SEND_STATUS_NONE;

// global iup handlers
static Ihandle *dialog, *topFrame, *bottomFrame, *statsFrame;
static Ihandle *statusLabel;
static Ihandle *filterText, *filterButton;
Ihandle *filterSelectList;
static Ihandle *profileSelectList, *profileSaveButton;
static Ihandle *processInput;
// timer to update icons
static Ihandle *stateIcon;
static Ihandle *timer;
static Ihandle *timeout = NULL;

// stats panel
static Ihandle *statsGlobalLabel, *statsModuleLabel, *statsBufferLabel;
static LONG statsPrevCaptured = 0;
static DWORD statsPrevTime = 0;
static int statsLastPps = 0;

void showStatus(const char *line);
static int uiOnDialogShow(Ihandle *ih, int state);
static int uiStopCb(Ihandle *ih);
static int uiStartCb(Ihandle *ih);
static int uiTimerCb(Ihandle *ih);
static int uiTimeoutCb(Ihandle *ih);
static int uiListSelectCb(Ihandle *ih, char *text, int item, int state);
static int uiFilterTextCb(Ihandle *ih);
static void uiSetupModule(Module *module, Ihandle *parent);
static int uiProfileSelectCb(Ihandle *ih, char *text, int item, int state);
static int uiProfileSaveCb(Ihandle *ih);
static void uiRefreshProfileList(void);
static void uiSyncModuleStates(void);

// config file parsing — supports config.json (preferred) and config.txt (legacy)
#define CONFIG_JSON_FILE "config.json"
#define CONFIG_TXT_FILE  "config.txt"
#define CONFIG_MAX_RECORDS 64
#define CONFIG_BUF_SIZE 8192
typedef struct {
    char* filterName;
    char* filterValue;
} filterRecord;
UINT filtersSize;
filterRecord filters[CONFIG_MAX_RECORDS] = {0};
char configBuf[CONFIG_BUF_SIZE+2];
BOOL parameterized = 0; // parameterized flag, means reading args from command line

// resolve exe directory and append filename into pathBuf
static void buildConfigPath(char *pathBuf, int bufSize, const char *filename) {
    char *p;
    GetModuleFileName(NULL, pathBuf, bufSize);
    p = strrchr(pathBuf, '\\');
    if (p == NULL) p = strrchr(pathBuf, '/');
    strcpy(p+1, filename);
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
        // scan for keys inside object bounds
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
            filters[filtersSize].filterName = namePtr;
            filters[filtersSize].filterValue = filterPtr;
            filtersSize++;
        }
    }

    LOG("Loaded %u records from config.json.", filtersSize);
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
EAT_SPACE:  while (isspace(*current)) { ++current; }
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
        filters[filtersSize].filterName = last;
        current += 1;
        while (isspace(*current)) { ++current; }
        last = current;
        current = strchr(last, '\n');
        if (!current) break;
        filters[filtersSize].filterValue = last;
        *current = '\0';
        if (*(current-1) == '\r') *(current-1) = 0;
        last = current = current + 1;
        ++filtersSize;
    } while (last && last - configBuf < CONFIG_BUF_SIZE);

    LOG("Loaded %u records from config.txt.", filtersSize);
    return filtersSize > 0;
}

// loading up filters — try config.json first, fall back to config.txt
void loadConfig() {
    char path[MSG_BUFSIZE];

    buildConfigPath(path, MSG_BUFSIZE, CONFIG_JSON_FILE);
    LOG("Trying config: %s", path);
    if (loadConfigJson(path)) return;

    buildConfigPath(path, MSG_BUFSIZE, CONFIG_TXT_FILE);
    LOG("Trying config: %s", path);
    if (loadConfigTxt(path)) return;

    LOG("Failed to load from config. Fill in a simple one.");
    filters[filtersSize].filterName = "loopback packets";
    filters[filtersSize].filterValue = "outbound and ip.DstAddr >= 127.0.0.1 and ip.DstAddr <= 127.255.255.255";
    filtersSize = 1;
}

void init(int argc, char* argv[]) {
    UINT ix;
    Ihandle *topVbox, *bottomVbox, *dialogVBox, *controlHbox, *profileHbox, *processHbox;
    Ihandle *noneIcon, *doingIcon, *errorIcon;
    char* arg_value = NULL;

    // fill in config
    loadConfig();

    // iup inits
    IupOpen(&argc, &argv);

    // this is so easy to get wrong so it's pretty worth noting in the program
    statusLabel = IupLabel("NOTICE: When capturing localhost (loopback) packets, you CAN'T include inbound criteria.\n"
        "Filters like 'udp' need to be 'udp and outbound' to work. See readme for more info.");
    IupSetAttribute(statusLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(statusLabel, "PADDING", "8x8");

    topFrame = IupFrame(
        topVbox = IupVbox(
            filterText = IupText(NULL),
            controlHbox = IupHbox(
                stateIcon = IupLabel(NULL),
                filterButton = IupButton("Start", NULL),
                IupFill(),
                IupLabel("Presets:  "),
                filterSelectList = IupList(NULL),
                NULL
            ),
            profileHbox = IupHbox(
                IupFill(),
                IupLabel("Profile:  "),
                profileSelectList = IupList(NULL),
                profileSaveButton = IupButton("Save", NULL),
                NULL
            ),
            processHbox = IupHbox(
                IupFill(),
                IupLabel("Process:  "),
                processInput = IupText(NULL),
                NULL
            ),
            NULL
        )
    );

    // parse arguments and set globals *before* setting up UI.
    // arguments can be read and set after callbacks are setup
    // FIXME as Release is built as WindowedApp, stdout/stderr won't show
    LOG("argc: %d", argc);
    if (argc > 1) {
        if (!parseArgs(argc, argv)) {
            fprintf(stderr, "invalid argument count. ensure you're using options as \"--drop on\"");
            exit(-1); // fail fast.
        }
        parameterized = 1;
    }

    IupSetAttribute(topFrame, "TITLE", "Filtering");
    IupSetAttribute(topFrame, "EXPAND", "HORIZONTAL");
    IupSetAttribute(filterText, "EXPAND", "HORIZONTAL");
    IupSetCallback(filterText, "VALUECHANGED_CB", (Icallback)uiFilterTextCb);
    IupSetAttribute(filterButton, "PADDING", "8x");
    IupSetCallback(filterButton, "ACTION", uiStartCb);
    IupSetAttribute(topVbox, "NCMARGIN", "4x4");
    IupSetAttribute(topVbox, "NCGAP", "4x2");
    IupSetAttribute(controlHbox, "ALIGNMENT", "ACENTER");

    // setup state icon
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");
    IupSetAttribute(stateIcon, "PADDING", "4x");

    // fill in options and setup callback
    IupSetAttribute(filterSelectList, "VISIBLECOLUMNS", "24");
    IupSetAttribute(filterSelectList, "DROPDOWN", "YES");
    for (ix = 0; ix < filtersSize; ++ix) {
        char ixBuf[4];
        sprintf(ixBuf, "%d", ix+1); // ! staring from 1, following lua indexing
        IupStoreAttribute(filterSelectList, ixBuf, filters[ix].filterName);
    }
    IupSetAttribute(filterSelectList, "VALUE", "1");
    IupSetCallback(filterSelectList, "ACTION", (Icallback)uiListSelectCb);
    // set filter text value since the callback won't take effect before main loop starts
    IupSetAttribute(filterText, "VALUE", filters[0].filterValue);

    // setup profile controls
    IupSetAttribute(profileHbox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(profileSelectList, "VISIBLECOLUMNS", "18");
    IupSetAttribute(profileSelectList, "DROPDOWN", "YES");
    IupSetAttribute(profileSelectList, "VALUE", "0");
    IupSetCallback(profileSelectList, "ACTION", (Icallback)uiProfileSelectCb);
    IupSetAttribute(profileSaveButton, "PADDING", "8x");
    IupSetCallback(profileSaveButton, "ACTION", uiProfileSaveCb);

    // load profiles and fill dropdown
    profilesLoad();
    uiRefreshProfileList();

    // setup process filter input
    IupSetAttribute(processHbox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(processInput, "VISIBLECOLUMNS", "18");
    IupSetAttribute(processInput, "TIP", "e.g. game.exe (empty = all)");
    if (parameterized) {
        setFromParameter(processInput, "VALUE", "process");
    }

    // functionalities frame
    bottomFrame = IupFrame(
        bottomVbox = IupVbox(
            NULL
        )
    );
    IupSetAttribute(bottomFrame, "TITLE", "Functions");
    IupSetAttribute(bottomVbox, "NCMARGIN", "4x4");
    IupSetAttribute(bottomVbox, "NCGAP", "4x2");

    // create icons
    noneIcon = IupImage(8, 8, icon8x8);
    doingIcon = IupImage(8, 8, icon8x8);
    errorIcon = IupImage(8, 8, icon8x8);
    IupSetAttribute(noneIcon, "0", "BGCOLOR");
    IupSetAttribute(noneIcon, "1", "224 224 224");
    IupSetAttribute(doingIcon, "0", "BGCOLOR");
    IupSetAttribute(doingIcon, "1", "109 170 44");
    IupSetAttribute(errorIcon, "0", "BGCOLOR");
    IupSetAttribute(errorIcon, "1", "208 70 72");
    IupSetHandle("none_icon", noneIcon);
    IupSetHandle("doing_icon", doingIcon);
    IupSetHandle("error_icon", errorIcon);

    // setup module uis
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        uiSetupModule(*(modules+ix), bottomVbox);
    }

    // stats panel
    {
        Ihandle *statsVbox;
        statsFrame = IupFrame(
            statsVbox = IupVbox(
                statsGlobalLabel = IupLabel("Captured: 0 (0/s)  Sent: 0"),
                statsModuleLabel = IupLabel(""),
                statsBufferLabel = IupLabel(""),
                NULL
            )
        );
        IupSetAttribute(statsFrame, "TITLE", "Statistics");
        IupSetAttribute(statsFrame, "EXPAND", "HORIZONTAL");
        IupSetAttribute(statsVbox, "NCMARGIN", "4x2");
        IupSetAttribute(statsVbox, "NCGAP", "2x1");
        IupSetAttribute(statsGlobalLabel, "EXPAND", "HORIZONTAL");
        IupSetAttribute(statsModuleLabel, "EXPAND", "HORIZONTAL");
        IupSetAttribute(statsBufferLabel, "EXPAND", "HORIZONTAL");
    }

    // dialog
    dialog = IupDialog(
        dialogVBox = IupVbox(
            topFrame,
            bottomFrame,
            statsFrame,
            statusLabel,
            NULL
        )
    );

    IupSetAttribute(dialog, "TITLE", "clumsy " CLUMSY_VERSION);
    IupSetAttribute(dialog, "SIZE", "480x"); // add padding manually to width
    IupSetAttribute(dialog, "RESIZE", "NO");
    IupSetCallback(dialog, "SHOW_CB", (Icallback)uiOnDialogShow);


    // global layout settings to affect childrens
    IupSetAttribute(dialogVBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(dialogVBox, "NCMARGIN", "4x4");
    IupSetAttribute(dialogVBox, "NCGAP", "4x2");

    // load scenario file if --scenario argument was given
    {
        char *scenarioPath = IupGetGlobal("scenario");
        if (scenarioPath) {
            scenarioLoad(scenarioPath);
            LOG("scenario file: %s (%s)", scenarioPath,
                scenarioIsLoaded() ? "loaded" : "failed to load");
        }
    }

    // apply --profile argument if given
    {
        char *profileName = IupGetGlobal("profile");
        if (profileName) {
            if (profileApply(profileName)) {
                uiSyncModuleStates();
                LOG("profile applied: %s", profileName);
            } else {
                LOG("profile not found: %s", profileName);
            }
        }
    }

    // start named pipe control server
    pipeServerStart();

    // setup timer
    timer = IupTimer();
    IupSetAttribute(timer, "TIME", STR(ICON_UPDATE_MS));
    IupSetCallback(timer, "ACTION_CB", uiTimerCb);

    // setup timeout of program
    arg_value = IupGetGlobal("timeout");
    if(arg_value != NULL)
    {
        char valueBuf[16];
        sprintf(valueBuf, "%s000", arg_value);  // convert from seconds to milliseconds

        timeout = IupTimer();
        IupStoreAttribute(timeout, "TIME", valueBuf);
        IupSetCallback(timeout, "ACTION_CB", uiTimeoutCb);
        IupSetAttribute(timeout, "RUN", "YES");
    }
}

void startup() {
    // initialize seed
    srand((unsigned int)time(NULL));

    // kickoff event loops
    IupShowXY(dialog, IUP_CENTER, IUP_CENTER);
    IupMainLoop();
    // ! main loop won't return until program exit
}

void cleanup() {
    statsLogStop();
    pipeServerStop();

    IupDestroy(timer);
    if (timeout) {
        IupDestroy(timeout);
    }

    IupClose();
    endTimePeriod(); // try close if not closing
}

// ui logics
void showStatus(const char *line) {
    IupStoreAttribute(statusLabel, "TITLE", line); 
}

static BOOL checkIsRunning() {
    //It will be closed and destroyed when programm terminates (according to MSDN).
    HANDLE hStartEvent = CreateEventW(NULL, FALSE, FALSE, L"Global\\CLUMSY_IS_RUNNING_EVENT_NAME");

    if (hStartEvent == NULL)
        return TRUE;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hStartEvent);
        hStartEvent = NULL;
        return TRUE;
    }

    return FALSE;
}


static int uiOnDialogShow(Ihandle *ih, int state) {
    // only need to process on show
    HWND hWnd;
    BOOL exit;
    HICON icon;
    HINSTANCE hInstance;
    if (state != IUP_SHOW) return IUP_DEFAULT;
    hWnd = (HWND)IupGetAttribute(ih, "HWND");
    hInstance = GetModuleHandle(NULL);

    // set application icon
    icon = LoadIcon(hInstance, "CLUMSY_ICON");
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);

    exit = checkIsRunning();
    if (exit) {
        MessageBox(hWnd, (LPCSTR)"Theres' already an instance of clumsy running.",
            (LPCSTR)"Aborting", MB_OK);
        return IUP_CLOSE;
    }

    // try elevate and decides whether to exit
    exit = tryElevate(hWnd, parameterized);

    if (!exit && parameterized) {
        setFromParameter(filterText, "VALUE", "filter");
        LOG("is parameterized, start filtering upon execution.");
        uiStartCb(filterButton);
    }

    return exit ? IUP_CLOSE : IUP_DEFAULT;
}

static int uiStartCb(Ihandle *ih) {
    char buf[MSG_BUFSIZE];
    char combinedFilter[2048];
    const char *userFilter;
    const char *procName;
    UNREFERENCED_PARAMETER(ih);

    userFilter = IupGetAttribute(filterText, "VALUE");
    procName = IupGetAttribute(processInput, "VALUE");

    // build combined filter: user filter + optional process port filter
    if (procName && procName[0] != '\0') {
        char procFragment[1024];
        char errMsg[MSG_BUFSIZE];
        int result = buildProcessFilter(procName, procFragment, sizeof(procFragment),
                                        errMsg, sizeof(errMsg));
        if (result < 0) {
            showStatus(errMsg);
            return IUP_DEFAULT;
        }
        if (result > 0) {
            snprintf(combinedFilter, sizeof(combinedFilter), "(%s)%s",
                     userFilter, procFragment);
            LOG("Process filter: %s (%d ports)", procName, result);
        } else {
            strncpy(combinedFilter, userFilter, sizeof(combinedFilter) - 1);
            combinedFilter[sizeof(combinedFilter) - 1] = '\0';
        }
    } else {
        strncpy(combinedFilter, userFilter, sizeof(combinedFilter) - 1);
        combinedFilter[sizeof(combinedFilter) - 1] = '\0';
    }

    if (divertStart(combinedFilter, buf) == 0) {
        showStatus(buf);
        return IUP_DEFAULT;
    }

    // successfully started
    if (procName && procName[0] != '\0') {
        snprintf(buf, MSG_BUFSIZE, "Started filtering for process '%s'. Enable functionalities to take effect.", procName);
        showStatus(buf);
    } else {
        showStatus("Started filtering. Enable functionalities to take effect.");
    }
    IupSetAttribute(filterText, "ACTIVE", "NO");
    IupSetAttribute(processInput, "ACTIVE", "NO");
    IupSetAttribute(filterButton, "TITLE", "Stop");
    IupSetCallback(filterButton, "ACTION", uiStopCb);
    IupSetAttribute(timer, "RUN", "YES");

    // start stats log if --stats-log was given
    {
        char *logPath = IupGetGlobal("stats-log");
        if (logPath) {
            char *intervalStr = IupGetGlobal("stats-interval");
            int interval = intervalStr ? atoi(intervalStr) : 1;
            statsLogStart(logPath, interval);
        }
    }

    // kick off scenario playback if one is loaded
    scenarioStart();

    return IUP_DEFAULT;
}

static int uiStopCb(Ihandle *ih) {
    int ix;
    UNREFERENCED_PARAMETER(ih);
    
    // try stopping
    scenarioStop();
    IupSetAttribute(filterButton, "ACTIVE", "NO");
    IupFlush(); // flush to show disabled state
    divertStop();

    IupSetAttribute(filterText, "ACTIVE", "YES");
    IupSetAttribute(processInput, "ACTIVE", "YES");
    IupSetAttribute(filterButton, "TITLE", "Start");
    IupSetAttribute(filterButton, "ACTIVE", "YES");
    IupSetCallback(filterButton, "ACTION", uiStartCb);

    // stop timer and clean up icons
    IupSetAttribute(timer, "RUN", "NO");
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        modules[ix]->processTriggered = 0; // use = here since is threads already stopped
        IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
    }
    sendState = SEND_STATUS_NONE;
    IupSetAttribute(stateIcon, "IMAGE", "none_icon");

    // stop stats log file
    statsLogStop();

    // reset stats
    statsReset();
    statsPrevCaptured = 0;
    statsPrevTime = 0;
    statsLastPps = 0;
    IupSetAttribute(statsGlobalLabel, "TITLE", "Captured: 0 (0/s)  Sent: 0");
    IupSetAttribute(statsModuleLabel, "TITLE", "");
    IupSetAttribute(statsBufferLabel, "TITLE", "");

    showStatus("Stopped. To begin again, edit criteria and click Start.");
    return IUP_DEFAULT;
}

static int uiToggleControls(Ihandle *ih, int state) {
    Ihandle *controls = (Ihandle*)IupGetAttribute(ih, CONTROLS_HANDLE);
    short *target = (short*)IupGetAttribute(ih, SYNCED_VALUE);
    int controlsActive = IupGetInt(controls, "ACTIVE");
    if (controlsActive && !state) {
        IupSetAttribute(controls, "ACTIVE", "NO");
        InterlockedExchange16(target, I2S(state));
    } else if (!controlsActive && state) {
        IupSetAttribute(controls, "ACTIVE", "YES");
        InterlockedExchange16(target, I2S(state));
    }

    return IUP_DEFAULT;
}

static int uiTimerCb(Ihandle *ih) {
    int ix;
    UNREFERENCED_PARAMETER(ih);

    // pipe "stop" command: close the application
    if (pipeStopRequested) {
        return IUP_CLOSE;
    }

    // advance scenario playback
    scenarioTick();

    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (modules[ix]->processTriggered) {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "doing_icon");
            InterlockedAnd16(&(modules[ix]->processTriggered), 0);
        } else {
            IupSetAttribute(modules[ix]->iconHandle, "IMAGE", "none_icon");
        }
    }

    // update stats panel
    {
        DWORD now = GetTickCount();
        LONG captured = statsCapturedTotal;
        LONG sent = statsSentTotal;
        DWORD elapsed = now - statsPrevTime;
        char buf[512];

        if (elapsed >= 1000) {
            statsLastPps = (int)((captured - statsPrevCaptured) * 1000 / (int)elapsed);
            statsPrevCaptured = captured;
            statsPrevTime = now;
        } else if (statsPrevTime == 0) {
            statsPrevTime = now;
            statsPrevCaptured = captured;
        }

        sprintf(buf, "Captured: %ld (%d/s)    Sent: %ld", captured, statsLastPps, sent);
        IupStoreAttribute(statsGlobalLabel, "TITLE", buf);

        // per-module affected counts (only show enabled or non-zero)
        {
            char mbuf[512];
            int pos = 0;
            for (ix = 0; ix < MODULE_CNT; ++ix) {
                LONG cnt = modules[ix]->affectedCount;
                if (cnt > 0 || *(modules[ix]->enabledFlag)) {
                    pos += sprintf(mbuf + pos, "%s:%ld  ", modules[ix]->displayName, cnt);
                }
            }
            if (pos == 0) sprintf(mbuf, "(no module active)");
            IupStoreAttribute(statsModuleLabel, "TITLE", mbuf);
        }

        // buffer stats
        {
            char bbuf[256];
            sprintf(bbuf, "Lag buf: %d    Jitter buf: %d    BW buf: %d  BW limit: %ldKB/s",
                lagGetBufSize(), jitterGetBufSize(), bandwidthGetBufSize(), bandwidthGetLimitKBps());
            IupStoreAttribute(statsBufferLabel, "TITLE", bbuf);
        }

        // write stats log row if active
        statsLogTick();
    }

    // update global send status icon
    switch (sendState)
    {
    case SEND_STATUS_NONE:
        IupSetAttribute(stateIcon, "IMAGE", "none_icon");
        break;
    case SEND_STATUS_SEND:
        IupSetAttribute(stateIcon, "IMAGE", "doing_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    case SEND_STATUS_FAIL:
        IupSetAttribute(stateIcon, "IMAGE", "error_icon");
        InterlockedAnd16(&sendState, SEND_STATUS_NONE);
        break;
    }

    return IUP_DEFAULT;
}

static int uiTimeoutCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    return IUP_CLOSE;
 }

static int uiListSelectCb(Ihandle *ih, char *text, int item, int state) {
    UNREFERENCED_PARAMETER(text);
    UNREFERENCED_PARAMETER(ih);
    if (state == 1) {
        IupSetAttribute(filterText, "VALUE", filters[item-1].filterValue);
    }
    return IUP_DEFAULT;
}

static int uiFilterTextCb(Ihandle *ih)  {
    UNREFERENCED_PARAMETER(ih);
    // unselect list
    IupSetAttribute(filterSelectList, "VALUE", "0");
    return IUP_DEFAULT;
}

static void uiSyncModuleStates(void) {
    int ix;
    for (ix = 0; ix < MODULE_CNT; ix++) {
        char handleKey[64];
        Ihandle *toggle, *controls;
        short enabled = *(modules[ix]->enabledFlag);

        sprintf(handleKey, "_MOD_TOGGLE_%s", modules[ix]->shortName);
        toggle = IupGetHandle(handleKey);
        sprintf(handleKey, "_MOD_CONTROLS_%s", modules[ix]->shortName);
        controls = IupGetHandle(handleKey);

        if (toggle && controls) {
            IupSetAttribute(toggle, "VALUE", enabled ? "ON" : "OFF");
            IupSetAttribute(controls, "ACTIVE", enabled ? "YES" : "NO");
        }
    }
}

static void uiRefreshProfileList(void) {
    int i, cnt;
    char ixBuf[4];
    // clear existing items
    IupSetAttribute(profileSelectList, "REMOVEITEM", "ALL");
    cnt = profileCount();
    for (i = 0; i < cnt; i++) {
        sprintf(ixBuf, "%d", i + 1);
        IupStoreAttribute(profileSelectList, ixBuf, profileGetName(i));
    }
    IupSetAttribute(profileSelectList, "VALUE", "0");
}

static int uiProfileSelectCb(Ihandle *ih, char *text, int item, int state) {
    UNREFERENCED_PARAMETER(ih);
    UNREFERENCED_PARAMETER(item);
    if (state == 1) {
        if (profileApply(text)) {
            char msg[MSG_BUFSIZE];
            uiSyncModuleStates();
            sprintf(msg, "Profile '%s' applied.", text);
            showStatus(msg);
        }
    }
    return IUP_DEFAULT;
}

static int uiProfileSaveCb(Ihandle *ih) {
    char name[48] = "";
    UNREFERENCED_PARAMETER(ih);
    if (IupGetParam("Save Profile", NULL, NULL,
                     "Profile name: %s\n", name)) {
        if (name[0] != '\0') {
            profileSaveCurrent(name);
            uiRefreshProfileList();
            {
                char msg[MSG_BUFSIZE];
                sprintf(msg, "Profile '%s' saved.", name);
                showStatus(msg);
            }
        }
    }
    return IUP_DEFAULT;
}

static void uiSetupModule(Module *module, Ihandle *parent) {
    Ihandle *groupBox, *toggle, *controls, *icon;
    groupBox = IupHbox(
        icon = IupLabel(NULL),
        toggle = IupToggle(module->displayName, NULL),
        IupFill(),
        controls = module->setupUIFunc(),
        NULL
    );
    IupSetAttribute(groupBox, "EXPAND", "HORIZONTAL");
    IupSetAttribute(groupBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(controls, "ALIGNMENT", "ACENTER");
    IupAppend(parent, groupBox);

    // set controls as attribute to toggle and enable toggle callback
    IupSetCallback(toggle, "ACTION", (Icallback)uiToggleControls);
    IupSetAttribute(toggle, CONTROLS_HANDLE, (char*)controls);
    IupSetAttribute(toggle, SYNCED_VALUE, (char*)module->enabledFlag);
    IupSetAttribute(controls, "ACTIVE", "NO"); // startup as inactive
    IupSetAttribute(controls, "NCGAP", "4"); // startup as inactive

    // set default icon
    IupSetAttribute(icon, "IMAGE", "none_icon");
    IupSetAttribute(icon, "PADDING", "4x");
    module->iconHandle = icon;

    // store toggle/controls handles for profile UI sync
    {
        char handleKey[64];
        sprintf(handleKey, "_MOD_TOGGLE_%s", module->shortName);
        IupSetHandle(handleKey, toggle);
        sprintf(handleKey, "_MOD_CONTROLS_%s", module->shortName);
        IupSetHandle(handleKey, controls);
    }

    // parameterize toggle
    if (parameterized) {
        setFromParameter(toggle, "VALUE", module->shortName);
    }
}

int main(int argc, char* argv[]) {
    LOG("Is Run As Admin: %d", IsRunAsAdmin());
    LOG("Is Elevated: %d", IsElevated());
    init(argc, argv);
    startup();
    cleanup();
    return 0;
}
