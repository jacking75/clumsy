// pcap export  (Phase 3.1)
//
// statslog.cpp keeps aggregate counters; this keeps the packets themselves so
// they can be opened in Wireshark. The classic libpcap file format is a 24-byte
// global header followed by (16-byte record header + raw packet) repeated, so
// no external library is needed.
//
// Link type is LINKTYPE_RAW (101): WinDivert hands us bare IPv4/IPv6 datagrams
// with no Ethernet framing, which is exactly what RAW means.
//
// Threading: pcapExportWriteStage() runs on the divert read/clock threads while
// POST /api/pcap/stop runs on an HTTP worker. Without a lock the HTTP thread
// could fclose() the handle while a divert thread is inside fwrite(), so the
// file state is guarded by pcapLock. The hot path first checks the lock-free
// pcapActive flag, which keeps the cost at zero while pcap is off.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define PCAP_MAGIC          0xa1b2c3d4u
#define PCAP_VERSION_MAJOR  2
#define PCAP_VERSION_MINOR  4
#define LINKTYPE_RAW        101
#define PCAP_SNAPLEN        65535

static CRITICAL_SECTION pcapLock;
static volatile short   pcapLockReady = 0;
static volatile short   pcapActive    = 0;   // lock-free fast path for the hot loop
static FILE          *pcapFile     = NULL;
static long           pcapPackets  = 0;   // guarded by pcapLock
static long           pcapBytes    = 0;   // guarded by pcapLock
static long           pcapMaxPkts  = 0;   // 0 = unlimited
static long           pcapMaxBytes = 0;   // 0 = unlimited
static int            pcapStage    = PCAP_STAGE_POST;
static char           pcapFilePath[MSG_BUFSIZE] = "";

#pragma pack(push, 1)
typedef struct {
    unsigned int   magic;
    unsigned short versionMajor;
    unsigned short versionMinor;
    int            thisZone;    // GMT offset, always 0 here
    unsigned int   sigFigs;
    unsigned int   snapLen;
    unsigned int   network;     // LINKTYPE_*
} PcapGlobalHeader;

typedef struct {
    unsigned int tsSec;
    unsigned int tsUsec;
    unsigned int inclLen;       // bytes stored
    unsigned int origLen;       // bytes on the wire
} PcapRecordHeader;
#pragma pack(pop)

// Microseconds since the unix epoch, for the pcap record timestamp.
static unsigned long long wallClockMicros(void) {
#if defined(_WIN32)
    // FILETIME is 100ns ticks since 1601-01-01; the unix epoch is
    // 11644473600 seconds later.
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return uli.QuadPart / 10ULL - 11644473600000000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL +
           (unsigned long long)(ts.tv_nsec / 1000);
#endif
}

// Resolve --pcap-stage into a bit mask. Defaults to POST: what actually leaves
// the machine after every module ran, which is what you want to inspect.
static int resolveStage(void) {
    const char *s = argGet("pcap-stage");
    if (!s) return PCAP_STAGE_POST;
    if (_stricmp(s, "pre")  == 0)  return PCAP_STAGE_PRE;
    if (_stricmp(s, "both") == 0)  return PCAP_STAGE_PRE | PCAP_STAGE_POST;
    return PCAP_STAGE_POST;
}

// Called once from main() before any thread exists. Lazy first-use creation
// would itself race between the HTTP and main threads.
void pcapExportInit(void) {
    if (pcapLockReady) return;
    InitializeCriticalSection(&pcapLock);
    pcapLockReady = 1;
}

// Closes the file. Caller must hold pcapLock.
static void closeFileLocked(void) {
    if (!pcapFile) return;
    InterlockedExchange16(&pcapActive, 0);
    fflush(pcapFile);
    fclose(pcapFile);
    pcapFile = NULL;
    INFO("pcap: closed '%s' after %ld packets (%ld bytes)",
         pcapFilePath, pcapPackets, pcapBytes);
}

int pcapExportStart(const char *path, long maxPackets, long maxBytes) {
    PcapGlobalHeader gh;
    const char *stageName;
    long pktLimit, byteLimit;

    if (!path || !path[0]) return 0;
    if (!pcapLockReady) return 0;   // pcapExportInit() was never called

    EnterCriticalSection(&pcapLock);
    closeFileLocked();

    pcapFile = fopen(path, "wb");
    if (!pcapFile) {
        LeaveCriticalSection(&pcapLock);
        INFO("pcap: cannot open '%s' for writing", path);
        return 0;
    }

    memset(&gh, 0, sizeof(gh));
    gh.magic        = PCAP_MAGIC;
    gh.versionMajor = PCAP_VERSION_MAJOR;
    gh.versionMinor = PCAP_VERSION_MINOR;
    gh.snapLen      = PCAP_SNAPLEN;
    gh.network      = LINKTYPE_RAW;
    if (fwrite(&gh, sizeof(gh), 1, pcapFile) != 1) {
        fclose(pcapFile);
        pcapFile = NULL;
        LeaveCriticalSection(&pcapLock);
        INFO("pcap: failed to write header to '%s'", path);
        return 0;
    }
    fflush(pcapFile);

    pcapPackets = 0;
    pcapBytes   = 0;
    pcapMaxPkts  = (maxPackets > 0) ? maxPackets : 0;
    pcapMaxBytes = (maxBytes   > 0) ? maxBytes   : 0;
    pcapStage    = resolveStage();
    strncpy(pcapFilePath, path, MSG_BUFSIZE - 1);
    pcapFilePath[MSG_BUFSIZE - 1] = '\0';

    // Snapshot for the log line so the console I/O happens outside the lock.
    stageName = (pcapStage == (PCAP_STAGE_PRE | PCAP_STAGE_POST)) ? "both" :
                (pcapStage == PCAP_STAGE_PRE) ? "pre" : "post";
    pktLimit  = pcapMaxPkts;
    byteLimit = pcapMaxBytes;

    // Set last: the divert threads use pcapActive as their lock-free gate, so
    // it must only go high once every field above is in place.
    InterlockedExchange16(&pcapActive, 1);
    LeaveCriticalSection(&pcapLock);

    INFO("pcap: writing to '%s' (stage=%s, maxPackets=%ld, maxBytes=%ld)",
         path, stageName, pktLimit, byteLimit);
    return 1;
}

void pcapExportStop(void) {
    if (!pcapLockReady) return;
    EnterCriticalSection(&pcapLock);
    closeFileLocked();
    LeaveCriticalSection(&pcapLock);
}

int  pcapExportIsActive(void) { return pcapActive; }
long pcapExportCount(void)    { return pcapPackets; }
long pcapExportBytes(void)    { return pcapBytes; }
const char* pcapExportPath(void) { return pcapFilePath; }

void pcapExportWriteStage(int stage, const char *packet, UINT len, BOOL outbound) {
    PcapRecordHeader rh;
    unsigned long long usSinceEpoch;
    UINT stored;
    int hitLimit = 0;

    UNREFERENCED_PARAMETER(outbound);

    // lock-free early out: costs one volatile read per packet when pcap is off
    if (!pcapActive || !packet || len == 0) return;

    EnterCriticalSection(&pcapLock);
    if (!pcapFile || (pcapStage & stage) == 0) {
        LeaveCriticalSection(&pcapLock);
        return;
    }

    // size limits - stop cleanly rather than filling the disk
    if ((pcapMaxPkts  > 0 && pcapPackets >= pcapMaxPkts) ||
        (pcapMaxBytes > 0 && pcapBytes   >= pcapMaxBytes)) {
        hitLimit = 1;
    }

    if (!hitLimit) {
        usSinceEpoch = wallClockMicros();

        stored = (len > PCAP_SNAPLEN) ? PCAP_SNAPLEN : len;

        rh.tsSec   = (unsigned int)(usSinceEpoch / 1000000ULL);
        rh.tsUsec  = (unsigned int)(usSinceEpoch % 1000000ULL);
        rh.inclLen = stored;
        rh.origLen = len;

        if (fwrite(&rh, sizeof(rh), 1, pcapFile) != 1 ||
            fwrite(packet, 1, stored, pcapFile) != stored) {
            INFO("pcap: write failed, closing '%s'", pcapFilePath);
            closeFileLocked();
            LeaveCriticalSection(&pcapLock);
            return;
        }

        pcapPackets++;
        pcapBytes += (long)stored;
    }

    if (hitLimit) {
        INFO("pcap: reached the configured limit, closing the file.");
        closeFileLocked();
    }
    LeaveCriticalSection(&pcapLock);
}
