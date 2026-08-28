#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

// Phase 4.5: platform.h supplies the Win32 vocabulary (typedefs, Interlocked*,
// tick counters, threads, critical sections) on both Windows and POSIX.
#include "platform.h"

// Phase 4.1: windivert.h is deliberately NOT included here. Only the Windows
// capture backend (divert.cpp, packetutil_win.cpp) may see it, so the modules
// compile unchanged against the Linux NFQUEUE backend.

#define CLUMSY_VERSION "0.4"
#define MSG_BUFSIZE 512
#define FILTER_BUFSIZE 1024
#define NAME_SIZE 16
#define MODULE_CNT 11
// main loop tick; also the Server-Sent Events push interval
#define CLOCK_TICK_MS 200
// kept for source compatibility with pre-Phase-2 call sites
#define ICON_UPDATE_MS CLOCK_TICK_MS

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
// (POSIX builds get these from platform.h instead)
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


// ---------------------------------------------------------------------------
// Console logging  (Phase 2.3)
//
// Both Debug and Release are ConsoleApp now, so everything goes to stdout and
// the old OutputDebugString path is gone.
//
//   INFO() — always printed. Banner, status changes, errors: what an operator
//            needs to see.
//   LOG()  — trace detail, gated at runtime by logVerbose. It is called once
//            per packet in the hot paths, so it stays off unless asked for:
//            Debug defaults to on, Release to off, `--verbose on|off` wins.
// ---------------------------------------------------------------------------
extern volatile short logVerbose;
void logPrintf(const char *fmt, ...);

#define INFO(fmt, ...) (logPrintf(fmt "\n", ##__VA_ARGS__))
#define LOG(fmt, ...)  do { if (logVerbose) logPrintf("%s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__); } while (0)

#ifdef _DEBUG
#define ABORT() assert(0)
// check for assert
#ifndef assert
// some how vs can't trigger debugger on assert, which is really stupid
#define assert(x) do {if (!(x)) {DebugBreak();} } while(0)
#endif
#else
#define ABORT()
#endif

// ---------------------------------------------------------------------------
// Platform-neutral packet metadata  (Phase 4.1)
//
// Everything the modules are allowed to know about a captured packet. The
// capture backend fills it in: WinDivert from WINDIVERT_ADDRESS, Linux from the
// NFQUEUE message header.
// ---------------------------------------------------------------------------
typedef struct {
    unsigned char outbound;   // 1 = leaving this host
    unsigned char ipVersion;  // 4 or 6
    unsigned char loopback;   // 1 = loopback traffic
    unsigned char impostor;   // 1 = this packet was injected, not captured
    unsigned int  ifIdx;      // interface index (0 when the backend has none)
    unsigned int  subIfIdx;   // sub-interface index
} PacketMeta;

// Opaque per-packet storage owned by the capture backend: WinDivert keeps the
// 80-byte WINDIVERT_ADDRESS it needs to re-inject, Linux keeps the NFQUEUE
// packet id. Kept inline rather than behind a pointer so the packet hot path
// stays at one allocation per packet.
#define PACKET_BACKEND_META_SIZE 80
typedef struct {
    alignas(8) unsigned char raw[PACKET_BACKEND_META_SIZE];
} PacketBackendMeta;

// package node
typedef struct _NODE {
    char *packet;
    UINT packetLen;
    PacketMeta meta;
    PacketBackendMeta backend;
    DWORD timestamp; // ! timestamp isn't filled when creating node since it's only needed for lag
    struct _NODE *prev, *next;
} PacketNode;

void initPacketNodeList();
// backendMeta may be NULL when the caller has nothing backend-specific to keep.
PacketNode* createNode(char* buf, UINT len, const PacketMeta *meta,
                       const void *backendMeta);
// Clones a captured packet for injection (duplicate.cpp). Goes through the
// backend so each one can decide what a clone means: WinDivert reuses the
// capture address, NFQUEUE marks it synthetic because a queued packet id can
// only be verdicted once.
PacketNode* cloneNode(const PacketNode *src);
void freeNode(PacketNode *node);
PacketNode* popNode(PacketNode *node);
PacketNode* insertBefore(PacketNode *node, PacketNode *target);
PacketNode* insertAfter(PacketNode *node, PacketNode *target);
PacketNode* appendNode(PacketNode *node);
short isListEmpty();


// key-value pair for module parameter serialization (used by profiles)
#define PARAM_KEY_SIZE 48
#define PARAM_VAL_SIZE 64
typedef struct { char key[PARAM_KEY_SIZE]; char val[PARAM_VAL_SIZE]; } ParamKV;

// Parameter metadata so the web UI can build its input form without knowing
// anything about individual modules  (Phase 2.2).
// type is one of:
//   "int"     — integer in [minVal, maxVal]
//   "float"   — real number in [minVal, maxVal]
//   "percent" — real number 0..100, passed to setParam as a percentage string
//   "bool"    — "true" / "false"
//   "action"  — write-only trigger; UI renders a button that posts true
typedef struct {
    const char *key;        // same key setParam takes (e.g. "lag-time")
    const char *label;      // human readable display name
    const char *type;
    double minVal, maxVal;  // input range (0, 0 when not applicable)
} ParamSpec;

// module
typedef struct {
    /*
     * Static module data
     */
    const char *displayName; // display name shown in ui
    const char *shortName; // single word name
    short *enabledFlag; // volatile short flag to determine enabled or not
    void (*startUp)(); // called when starting up the module
    void (*closeDown)(PacketNode *head, PacketNode *tail); // called when starting up the module
    short (*process)(PacketNode *head, PacketNode *tail);
    /* control API: set a named parameter from a string value.
     * key uses the same CLI-style names as --module-param (e.g. "lag-time").
     * Chance values are passed as percentage strings ("10.0" = 10%).
     * Returns 1 if key was recognised, 0 otherwise. NULL if module has no params. */
    int (*setParam)(const char *key, const char *value);
    /* profile API: read current parameter values.
     * Fills kv[] with key-value pairs (same key format as setParam).
     * Returns number of pairs written. NULL if module has no params. */
    int (*getParams)(ParamKV *kv, int maxKv);
    /* form metadata for the web UI; serialized as-is by GET /api/modules */
    const ParamSpec *paramSpecs;
    int paramSpecCount;
    /*
     * Flags used during program excution. Need to be re initialized on each run
     */
    short lastEnabled; // if it is enabled on last run
    short processTriggered; // whether this module has been triggered in last step
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


// Console status line (main.cpp) — replaces the old IUP status label.
void showStatus(const char* line);
const char* appStatusLine(void);

// WinDivert
int divertStart(const char * filter, char buf[]);
void divertStop();

// Real-time statistics (divert.cpp)
extern volatile LONG statsCapturedTotal;
extern volatile LONG statsSentTotal;
void statsReset(void);

// Module buffer getters for stats panel
int lagGetBufSize(void);
int jitterGetBufSize(void);
int bandwidthGetBufSize(void);
LONG bandwidthGetLimitKBps(void);

// Stats log file output  (statslog.cpp)
void statsLogStart(const char *path, int intervalSec);
void statsLogStop(void);
void statsLogTick(void);

// Process-based filter  (procfilter.cpp)
// Resolves processName to local ports and writes a WinDivert filter fragment
// like " and (tcp.SrcPort == X or tcp.DstPort == X or ...)" into filterBuf.
// Returns port count on success, 0 if name is empty, -1 on error (errBuf filled).
int buildProcessFilter(const char *processName, char *filterBuf, int bufSize,
                       char *errBuf, int errSize);

// Control API transport  (pipe.cpp)
// Windows: Named Pipe \\.\pipe\clumsy. POSIX: Unix domain socket.
// Both speak the identical JSON protocol via controlDispatchJson().
void pipeServerStart(void);
void pipeServerStop(void);
#if !defined(_WIN32)
// Socket path actually bound, or "" when the server did not start.
const char* pipeServerPath(void);
#endif
extern volatile short pipeStopRequested; // set by "stop" command; checked in main loop

// Shared module-KV helper  (utils.cpp)
// Applies one key=value pair to the matching module (enable/disable or setParam).
int applyModuleKV(const char *key, const char *value);

// Scenario scripting  (scenario.cpp)
void scenarioLoad(const char *path);
void scenarioStart(void);
void scenarioStop(void);
void scenarioTick(void);
int  scenarioIsLoaded(void);
int  scenarioStepCount(void);
int  scenarioIsActive(void);

// Profile save/load  (profile.cpp)
void        profilesLoad(void);          // loads profiles.json from exe directory
int         profileApply(const char *name); // 1=ok, 0=not found
int         profileSaveCurrent(const char *name); // save current module state, 1=ok
int         profileCount(void);
const char* profileGetName(int ix);      // 0-indexed

// pcap export  (pcapexport.cpp, Phase 3.1)
// Packets can be dumped as captured (PRE, before any module ran) or as they go
// back on the wire (POST, after every module). --pcap-stage picks which.
#define PCAP_STAGE_PRE  1
#define PCAP_STAGE_POST 2
void pcapExportInit(void);   // call once from main() before threads start
int  pcapExportStart(const char *path, long maxPackets, long maxBytes);
void pcapExportStop(void);
int  pcapExportIsActive(void);
long pcapExportCount(void);
long pcapExportBytes(void);
const char* pcapExportPath(void);
// Called from the divert threads inside the capture mutex.
void pcapExportWriteStage(int stage, const char *packet, UINT len, BOOL outbound);

// Session report  (report.cpp, Phase 3.3)
void reportInit(void);       // call once from main() before threads start
void reportSessionStart(const char *filterText);
void reportSessionStop(void);
void reportNoteEvent(const char *text);
// Sampling hook, called once per main-loop tick while capturing.
void reportTick(void);
// Writes an HTML session report. Returns 1 on success.
int  reportWriteHtml(const char *path);
// Same report rendered into a caller-owned buffer (used by the web download).
// Returns the number of bytes written, or 0 on failure.
int  reportRenderHtml(char *buf, int bufSize);

// Plugin modules  (plugin.cpp, Phase 3.6)
int  pluginLoadDir(const char *dir);
void pluginUnloadAll(void);

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

// ---------------------------------------------------------------------------
// Capture backend hooks  (Phase 4.2)
//
// Implemented by divert.cpp (Windows) and divert_linux.cpp (NFQUEUE).
// ---------------------------------------------------------------------------
// Called by freeNode() just before a node is released. NFQUEUE must account for
// every packet it was handed - a queued id that never receives a verdict stalls
// the kernel queue - so the Linux backend issues NF_DROP here for any packet no
// module chose to send. No-op on Windows, where dropping is simply not sending.
void packetBackendOnFree(PacketNode *node);
// Prepares a freshly cloned node's backend state. See cloneNode().
void packetBackendPrepareClone(const PacketNode *src, PacketNode *dst);

// ---------------------------------------------------------------------------
// Packet inspection helpers  (Phase 4.1)
//
// Modules go through these instead of calling a capture backend directly, so
// the same module source compiles against WinDivert and libnetfilter_queue.
// Implemented per backend in packetutil_win.cpp / packetutil_linux.cpp.
// ---------------------------------------------------------------------------
// Locates the transport payload inside packet. Returns 1 and fills
// *payload / *payloadLen on success, 0 when there is no payload to touch.
int  packetGetPayload(char *packet, UINT len, char **payload, UINT *payloadLen);
// Recomputes IP/TCP/UDP checksums in place after a modification.
void packetRecalcChecksums(char *packet, UINT len);
// Sets the TCP RST flag and fixes checksums. Returns 1 if the packet was TCP.
int  packetSetTcpRst(char *packet, UINT len);
// Smallest packet that can carry an IPv4 TCP header.
UINT packetMinTcpSize(void);

// shared setParam helpers so every module handles "<name>-inbound"/"-outbound"
// the same way  (utils.cpp)
int  parseBoolValue(const char *value);
// Clamp helpers used by setParam implementations.
short clampShort(int v, int lo, int hi);
LONG  clampLong(LONG v, LONG lo, LONG hi);


// wraped timeBegin/EndPeriod to keep calling safe and end when exit
#define TIMER_RESOLUTION 4
void startTimePeriod();
void endTimePeriod();

// elevate
BOOL IsElevated();
BOOL IsRunAsAdmin();
// Relaunch self elevated. Returns TRUE when the caller should exit (a new
// elevated instance was launched, or elevation is impossible).
BOOL tryElevate(BOOL silent);

// ---------------------------------------------------------------------------
// CLI argument store (utils.cpp) — replaces the old IupStoreGlobal/IupGetGlobal
// ---------------------------------------------------------------------------
void        argSet(const char *key, const char *value);
const char* argGet(const char *key);                 // NULL when not given
int         argGetInt(const char *key, int defVal);
BOOL        parseArgs(int argc, char* argv[]);
extern BOOL parameterized; // true when any --key value pair was given

// ---------------------------------------------------------------------------
// Application control surface, implemented in main.cpp and shared by the
// Named Pipe server, the HTTP server and the scenario runner  (Phase 2.5)
// ---------------------------------------------------------------------------
// Starts capture with the given filter. procName may be NULL/"" for all
// processes. Returns 1 on success, 0 on failure with errBuf filled.
int         appStartCapture(const char *filter, const char *procName,
                            char *errBuf, int errSize);
void        appStopCapture(void);
int         appIsCapturing(void);
void        appRequestQuit(void);
const char* appCurrentFilter(void);
const char* appCurrentProcess(void);
DWORD       appCaptureElapsedMs(void);

// filter presets loaded from config.json / config.txt
int         appPresetCount(void);
const char* appPresetName(int ix);
const char* appPresetFilter(int ix);
// Appends a preset and rewrites config.json. Returns 1 on success.
int         appPresetSave(const char *name, const char *filter);
