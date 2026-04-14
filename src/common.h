#pragma once
#include <stdio.h>
#include <assert.h>
#include "iup.h"
#include "windivert.h"

#define CLUMSY_VERSION "0.3"
#define MSG_BUFSIZE 512
#define FILTER_BUFSIZE 1024
#define NAME_SIZE 16
#define MODULE_CNT 11
#define ICON_UPDATE_MS 200

#define CONTROLS_HANDLE "__CONTROLS_HANDLE"
#define SYNCED_VALUE "__SYNCED_VALUE"
#define INTEGER_MAX "__INTEGER_MAX"
#define INTEGER_MIN "__INTEGER_MIN"
#define FIXED_MAX "__FIXED_MAX"
#define FIXED_MIN "__FIXED_MIN"
#define FIXED_EPSILON 0.01

// workaround stupid vs2012 runtime check.
// it would show even when seeing explicit "(short)(i);"
#define I2S(x) ((short)((x) & 0xFFFF))


#ifdef __MINGW32__
#define INLINE_FUNCTION __inline__
#else
#define INLINE_FUNCTION __inline
#endif


// my mingw seems missing some of the functions
// undef all mingw linked interlock* and use __atomic gcc builtins
#ifdef __MINGW32__
// and 16 seems to be broken
#ifdef InterlockedAnd16
#undef InterlockedAnd16
#endif
#define InterlockedAnd16(p, val) (__atomic_and_fetch((short*)(p), (val), __ATOMIC_SEQ_CST))

#ifdef InterlockedExchange16
#undef InterlockedExchange16
#endif
#define InterlockedExchange16(p, val) (__atomic_exchange_n((short*)(p), (val), __ATOMIC_SEQ_CST))

#ifdef InterlockedIncrement16
#undef InterlockedIncrement16
#endif
#define InterlockedIncrement16(p) (__atomic_add_fetch((short*)(p), 1, __ATOMIC_SEQ_CST))

#ifdef InterlockedDecrement16
#undef InterlockedDecrement16
#endif
#define InterlockedDecrement16(p) (__atomic_sub_fetch((short*)(p), 1, __ATOMIC_SEQ_CST))

#endif



#ifdef _DEBUG
#define ABORT() assert(0)
#ifdef __MINGW32__
#define LOG(fmt, ...) (printf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__))
#else
static void VsLog(const char* pFmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, pFmt);
    vsprintf_s(buf, 1024, pFmt, args);
    va_end(args);

    OutputDebugString(buf);
}

#define LOG(fmt, ...) (VsLog(__FUNCTION__ ": " fmt "\n", ##__VA_ARGS__))
#endif

// check for assert
#ifndef assert
// some how vs can't trigger debugger on assert, which is really stupid
#define assert(x) do {if (!(x)) {DebugBreak();} } while(0)
#endif


#else
#define LOG(fmt, ...)
#define ABORT()
//#define assert(x)
#endif

// package node
typedef struct _NODE {
    char *packet;
    UINT packetLen;
    WINDIVERT_ADDRESS addr;
    DWORD timestamp; // ! timestamp isn't filled when creating node since it's only needed for lag
    struct _NODE *prev, *next;
} PacketNode;

void initPacketNodeList();
PacketNode* createNode(char* buf, UINT len, WINDIVERT_ADDRESS *addr);
void freeNode(PacketNode *node);
PacketNode* popNode(PacketNode *node);
PacketNode* insertBefore(PacketNode *node, PacketNode *target);
PacketNode* insertAfter(PacketNode *node, PacketNode *target);
PacketNode* appendNode(PacketNode *node);
short isListEmpty();

// shared ui handlers
int uiSyncChance(Ihandle *ih);
int uiSyncToggle(Ihandle *ih, int state);
int uiSyncInteger(Ihandle *ih);
int uiSyncFixed(Ihandle *ih);
int uiSyncInt32(Ihandle *ih);


// key-value pair for module parameter serialization (used by profiles)
#define PARAM_KEY_SIZE 48
#define PARAM_VAL_SIZE 64
typedef struct { char key[PARAM_KEY_SIZE]; char val[PARAM_VAL_SIZE]; } ParamKV;

// module
typedef struct {
    /*
     * Static module data
     */
    const char *displayName; // display name shown in ui
    const char *shortName; // single word name
    short *enabledFlag; // volatile short flag to determine enabled or not
    Ihandle* (*setupUIFunc)(); // return hbox as controls group
    void (*startUp)(); // called when starting up the module
    void (*closeDown)(PacketNode *head, PacketNode *tail); // called when starting up the module
    short (*process)(PacketNode *head, PacketNode *tail);
    /* pipe API: set a named parameter from a string value.
     * key uses the same CLI-style names as --module-param (e.g. "lag-time").
     * Chance values are passed as percentage strings ("10.0" = 10%).
     * Returns 1 if key was recognised, 0 otherwise. NULL if module has no params. */
    int (*setParam)(const char *key, const char *value);
    /* profile API: read current parameter values.
     * Fills kv[] with key-value pairs (same key format as setParam).
     * Returns number of pairs written. NULL if module has no params. */
    int (*getParams)(ParamKV *kv, int maxKv);
    /*
     * Flags used during program excution. Need to be re initialized on each run
     */
    short lastEnabled; // if it is enabled on last run
    short processTriggered; // whether this module has been triggered in last step
    Ihandle *iconHandle; // store the icon to be updated
    volatile LONG affectedCount; // cumulative packets affected by this module
} Module;

extern Module lagModule;
extern Module jitterModule;
extern Module dropModule;
extern Module burstlossModule;
extern Module blackoutModule;
extern Module throttleModule;
extern Module oodModule;
extern Module dupModule;
extern Module tamperModule;
extern Module resetModule;
extern Module bandwidthModule;
extern Module* modules[MODULE_CNT]; // all modules in a list

// status for sending packets, 
#define SEND_STATUS_NONE 0
#define SEND_STATUS_SEND 1
#define SEND_STATUS_FAIL -1
extern volatile short sendState;


// Iup GUI
void showStatus(const char* line);

// WinDivert
int divertStart(const char * filter, char buf[]);
void divertStop();

// Real-time statistics (divert.c)
extern volatile LONG statsCapturedTotal;
extern volatile LONG statsSentTotal;
void statsReset(void);

// Module buffer getters for stats panel
int lagGetBufSize(void);
int jitterGetBufSize(void);
int bandwidthGetBufSize(void);
LONG bandwidthGetLimitKBps(void);

// Stats log file output  (statslog.c)
void statsLogStart(const char *path, int intervalSec);
void statsLogStop(void);
void statsLogTick(void);

// Process-based filter  (procfilter.c)
// Resolves processName to local ports and writes a WinDivert filter fragment
// like " and (tcp.SrcPort == X or tcp.DstPort == X or ...)" into filterBuf.
// Returns port count on success, 0 if name is empty, -1 on error (errBuf filled).
int buildProcessFilter(const char *processName, char *filterBuf, int bufSize,
                       char *errBuf, int errSize);

// Named Pipe control API  (pipe.c)
void pipeServerStart(void);
void pipeServerStop(void);
extern volatile short pipeStopRequested; // set by "stop" command; checked in main timer

// Shared module-KV helper  (utils.c)
// Applies one key=value pair to the matching module (enable/disable or setParam).
int applyModuleKV(const char *key, const char *value);

// Scenario scripting  (scenario.c)
void scenarioLoad(const char *path);
void scenarioStart(void);
void scenarioStop(void);
void scenarioTick(void);
int  scenarioIsLoaded(void);

// Profile save/load  (profile.c)
void        profilesLoad(void);          // loads profiles.json from exe directory
int         profileApply(const char *name); // 1=ok, 0=not found
int         profileSaveCurrent(const char *name); // save current module state, 1=ok
int         profileCount(void);
const char* profileGetName(int ix);      // 0-indexed

// utils
// STR to convert int macro to string
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

short calcChance(short chance);

// inline helper for inbound outbound check
static INLINE_FUNCTION
BOOL checkDirection(BOOL outboundPacket, short handleInbound, short handleOutbound) {
    return (handleInbound && !outboundPacket) || (handleOutbound && outboundPacket);
}


// wraped timeBegin/EndPeriod to keep calling safe and end when exit
#define TIMER_RESOLUTION 4
void startTimePeriod();
void endTimePeriod();

// elevate
BOOL IsElevated();
BOOL IsRunAsAdmin();
BOOL tryElevate(HWND hWnd, BOOL silent);

// icons
extern const unsigned char icon8x8[8*8];

// parameterized
extern BOOL parameterized;
void setFromParameter(Ihandle *ih, const char *field, const char *key);
BOOL parseArgs(int argc, char* argv[]);

