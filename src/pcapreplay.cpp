// pcap replay  (T7)
//
// The inverse of pcapexport.cpp: reads a libpcap file back and re-injects the
// packets through the capture backend, preserving the recorded inter-packet
// timing. A bug that only reproduces under one exact traffic sequence becomes
// a repeatable test rather than a story about what happened once.
//
// The file is streamed, never loaded: a capture worth replaying is routinely
// hundreds of megabytes, and only one packet is ever in memory.
//
// Link types. The file clumsy itself writes is LINKTYPE_RAW (101), bare IP
// with no framing, which is exactly what the injection path wants. Files from
// Wireshark or tcpdump are usually LINKTYPE_ETHERNET (1), so the 14-byte
// Ethernet header is stripped when the EtherType says IPv4 or IPv6. Anything
// else is rejected at open time with a clear message rather than injecting
// nonsense.
//
// Threading: one replay thread does all the reading, sleeping and injecting.
// pcapReplayStop() sets the stop flag and joins that thread before touching
// any backend state, so the lazily-opened injection handle/sockets are only
// ever used from a single thread.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define PCAP_MAGIC          0xa1b2c3d4u
#define PCAP_MAGIC_SWAPPED  0xd4c3b2a1u
// Nanosecond-resolution variants written by newer tcpdump.
#define PCAP_MAGIC_NS       0xa1b23c4du
#define PCAP_MAGIC_NS_SWAP  0x4d3cb2a1u

#define LINKTYPE_NULL       0
#define LINKTYPE_ETHERNET   1
#define LINKTYPE_RAW        101
#define LINKTYPE_IPV4       228
#define LINKTYPE_IPV6       229

#define REPLAY_MAX_PACKET   65535
#define ETHER_HEADER_LEN    14

#pragma pack(push, 1)
typedef struct {
    unsigned int   magic;
    unsigned short versionMajor;
    unsigned short versionMinor;
    int            thisZone;
    unsigned int   sigFigs;
    unsigned int   snapLen;
    unsigned int   network;
} ReplayGlobalHeader;

typedef struct {
    unsigned int tsSec;
    unsigned int tsUsec;
    unsigned int inclLen;
    unsigned int origLen;
} ReplayRecordHeader;
#pragma pack(pop)

static CRITICAL_SECTION replayLock;
static volatile short   replayLockReady = 0;
static volatile short   replayActive    = 0;
static volatile short   replayStop      = 0;
static HANDLE           replayThread    = NULL;

// Counters. Written by the replay thread, read by HTTP workers; LONG through
// the Interlocked helpers, same contract as the other stats counters.
static volatile LONG replayReadCount   = 0;
static volatile LONG replaySentCount   = 0;
static volatile LONG replayFailedCount = 0;

static char   replayFilePath[MSG_BUFSIZE] = "";
static double replaySpeed = 1.0;
static int    replayLoop  = 0;

void pcapReplayInit(void) {
    if (replayLockReady) return;
    InitializeCriticalSection(&replayLock);
    replayLockReady = 1;
}

static unsigned int swap32(unsigned int v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >>  8) | ((v & 0xFF000000u) >> 24);
}

// Opens the file and validates the global header. Returns the FILE* and fills
// *swapped / *linkType, or NULL with errBuf set.
static FILE* openAndValidate(const char *path, int *swapped, unsigned int *linkType,
                             char *errBuf, int errSize) {
    ReplayGlobalHeader gh;
    FILE *f = fopen(path, "rb");

    if (!f) {
        snprintf(errBuf, errSize, "cannot open '%s' for reading", path);
        return NULL;
    }
    if (fread(&gh, sizeof(gh), 1, f) != 1) {
        snprintf(errBuf, errSize, "'%s' is too short to be a pcap file", path);
        fclose(f);
        return NULL;
    }

    if (gh.magic == PCAP_MAGIC || gh.magic == PCAP_MAGIC_NS) {
        *swapped = 0;
    } else if (gh.magic == PCAP_MAGIC_SWAPPED || gh.magic == PCAP_MAGIC_NS_SWAP) {
        *swapped = 1;
    } else {
        // pcapng starts with 0x0A0D0D0A and is a different format entirely.
        snprintf(errBuf, errSize,
                 "'%s' is not a classic libpcap file (magic 0x%08x). "
                 "pcapng is not supported; save as 'Wireshark/tcpdump/... - pcap'.",
                 path, gh.magic);
        fclose(f);
        return NULL;
    }

    *linkType = *swapped ? swap32(gh.network) : gh.network;

    if (*linkType != LINKTYPE_RAW && *linkType != LINKTYPE_ETHERNET &&
        *linkType != LINKTYPE_IPV4 && *linkType != LINKTYPE_IPV6) {
        snprintf(errBuf, errSize,
                 "'%s' has link type %u; only RAW (101), Ethernet (1), "
                 "IPv4 (228) and IPv6 (229) can be replayed.",
                 path, *linkType);
        fclose(f);
        return NULL;
    }
    return f;
}

// Strips the framing the link type carries, leaving a bare IP packet.
// Returns the offset to the IP header, or -1 when the frame is not IP.
static int ipOffsetFor(unsigned int linkType, const char *buf, unsigned int len) {
    if (linkType == LINKTYPE_ETHERNET) {
        unsigned short ethType;
        if (len <= ETHER_HEADER_LEN) return -1;
        ethType = (unsigned short)(((unsigned char)buf[12] << 8) | (unsigned char)buf[13]);
        if (ethType != 0x0800 && ethType != 0x86DD) return -1;  // ARP, VLAN, ...
        return ETHER_HEADER_LEN;
    }
    return 0;   // RAW / IPV4 / IPV6 are already bare IP
}

static DWORD WINAPI replayThreadProc(LPVOID arg) {
    char *buf;
    int swapped = 0;
    unsigned int linkType = LINKTYPE_RAW;
    char err[MSG_BUFSIZE] = "";
    double speed;
    int loopForever;

    UNREFERENCED_PARAMETER(arg);

    buf = (char*)malloc(REPLAY_MAX_PACKET);
    if (!buf) {
        INFO("replay: out of memory");
        InterlockedExchange16(&replayActive, 0);
        return 0;
    }

    speed       = (replaySpeed > 0.0) ? replaySpeed : 1.0;
    loopForever = replayLoop;

    do {
        FILE *f = openAndValidate(replayFilePath, &swapped, &linkType, err, sizeof(err));
        unsigned long long firstTs = 0;
        DWORD wallStart;
        int first = 1;

        if (!f) {
            INFO("replay: %s", err);
            break;
        }
        wallStart = GetTickCount();

        while (!replayStop) {
            ReplayRecordHeader rh;
            unsigned int inclLen, tsSec, tsUsec;
            unsigned long long ts;
            int off;

            if (fread(&rh, sizeof(rh), 1, f) != 1) break;   // clean end of file

            inclLen = swapped ? swap32(rh.inclLen) : rh.inclLen;
            tsSec   = swapped ? swap32(rh.tsSec)   : rh.tsSec;
            tsUsec  = swapped ? swap32(rh.tsUsec)  : rh.tsUsec;

            if (inclLen == 0 || inclLen > REPLAY_MAX_PACKET) {
                INFO("replay: record %ld claims %u bytes; file is corrupt, stopping.",
                     (long)replayReadCount, inclLen);
                break;
            }
            if (fread(buf, 1, inclLen, f) != inclLen) break;   // truncated tail

            InterlockedIncrement(&replayReadCount);

            ts = (unsigned long long)tsSec * 1000000ULL + tsUsec;
            if (first) {
                firstTs = ts;
                first = 0;
            }

            // Pace against the recorded timeline rather than sleeping a fixed
            // gap per packet: sleeping between packets accumulates the cost of
            // every injection and slowly drifts behind the original capture.
            {
                double targetMs = (ts >= firstTs)
                    ? ((double)(ts - firstTs) / 1000.0) / speed
                    : 0.0;
                DWORD elapsed = GetTickCount() - wallStart;
                if (targetMs > (double)elapsed) {
                    double waitMs = targetMs - (double)elapsed;
                    // Wake often enough that a stop request is honoured
                    // promptly even in the middle of a long idle gap.
                    while (waitMs > 0.0 && !replayStop) {
                        DWORD chunk = (waitMs > 100.0) ? 100 : (DWORD)waitMs;
                        if (chunk == 0) chunk = 1;
                        Sleep(chunk);
                        waitMs -= (double)chunk;
                    }
                }
            }
            if (replayStop) break;

            off = ipOffsetFor(linkType, buf, inclLen);
            if (off < 0) {
                // Non-IP frame (ARP, VLAN tagged, ...). Counted, not injected.
                InterlockedIncrement(&replayFailedCount);
                continue;
            }

            if (packetBackendInject(buf + off, inclLen - (unsigned int)off, TRUE)) {
                InterlockedIncrement(&replaySentCount);
            } else {
                InterlockedIncrement(&replayFailedCount);
            }
        }

        fclose(f);
    } while (loopForever && !replayStop);

    free(buf);

    INFO("replay: finished - %ld read, %ld injected, %ld skipped",
         (long)replayReadCount, (long)replaySentCount, (long)replayFailedCount);

    InterlockedExchange16(&replayActive, 0);
    return 0;
}

int pcapReplayStart(const char *path, double speed, int loopForever,
                    char *errBuf, int errSize) {
    int swapped = 0;
    unsigned int linkType = 0;
    FILE *probe;

    if (errBuf && errSize > 0) errBuf[0] = '\0';
    if (!path || !path[0]) {
        if (errBuf) snprintf(errBuf, errSize, "no replay file given");
        return 0;
    }
    if (!replayLockReady) {
        if (errBuf) snprintf(errBuf, errSize, "replay subsystem is not initialised");
        return 0;
    }

    EnterCriticalSection(&replayLock);
    if (replayActive) {
        LeaveCriticalSection(&replayLock);
        if (errBuf) snprintf(errBuf, errSize, "a replay is already running");
        return 0;
    }

    // Validate before spawning the thread, so a bad path or an unsupported
    // link type comes back as an error on this call instead of a log line the
    // caller never sees.
    probe = openAndValidate(path, &swapped, &linkType,
                            errBuf ? errBuf : NULL, errBuf ? errSize : 0);
    if (!probe) {
        LeaveCriticalSection(&replayLock);
        return 0;
    }
    fclose(probe);

    // A finished run leaves its thread handle behind; release it before the
    // next one overwrites the slot.
    if (replayThread) {
        CloseHandle(replayThread);
        replayThread = NULL;
    }

    strncpy(replayFilePath, path, MSG_BUFSIZE - 1);
    replayFilePath[MSG_BUFSIZE - 1] = '\0';
    replaySpeed = (speed > 0.0) ? speed : 1.0;
    replayLoop  = loopForever ? 1 : 0;

    InterlockedExchange(&replayReadCount, 0);
    InterlockedExchange(&replaySentCount, 0);
    InterlockedExchange(&replayFailedCount, 0);
    InterlockedExchange16(&replayStop, 0);
    InterlockedExchange16(&replayActive, 1);

    replayThread = CreateThread(NULL, 0, replayThreadProc, NULL, 0, NULL);
    if (!replayThread) {
        InterlockedExchange16(&replayActive, 0);
        LeaveCriticalSection(&replayLock);
        if (errBuf) snprintf(errBuf, errSize, "failed to create the replay thread");
        return 0;
    }
    LeaveCriticalSection(&replayLock);

    INFO("replay: playing '%s' at %.2fx%s (link type %u)",
         path, replaySpeed, replayLoop ? ", looping" : "", linkType);
    return 1;
}

void pcapReplayStop(void) {
    HANDLE t;

    if (!replayLockReady) return;

    EnterCriticalSection(&replayLock);
    InterlockedExchange16(&replayStop, 1);
    t = replayThread;
    replayThread = NULL;
    LeaveCriticalSection(&replayLock);

    if (t) {
        // Join before closing the backend: the replay thread is the only user
        // of the injection handle, and closing it underneath would be a
        // use-after-free.
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
    InterlockedExchange16(&replayActive, 0);
    packetBackendInjectClose();
}

int  pcapReplayIsActive(void) { return replayActive; }
long pcapReplayRead(void)     { return replayReadCount; }
long pcapReplaySent(void)     { return replaySentCount; }
long pcapReplayFailed(void)   { return replayFailedCount; }
const char* pcapReplayPath(void) { return replayFilePath; }
