// Platform compatibility layer  (Phase 4.5)
//
// clumsy was written against Win32 and the port keeps that vocabulary rather
// than rewriting every module: this header re-expresses the handful of Win32
// primitives the code actually uses (typedefs, Interlocked*, tick counters,
// threads, critical sections) in POSIX terms.
//
// The alternative - converting every module to std::atomic/std::thread/std::chrono -
// would have touched the whole packet hot path for no functional gain, and
// docs/CODING_STYLE.md section 1 explicitly rules that out for existing code.
//
// Anything genuinely platform-specific (packet capture, UAC/capabilities,
// process lookup) lives in a *_win.cpp / *_linux.cpp pair instead.
#pragma once

#if defined(_WIN32)

#include <Windows.h>

#else   // ---------------------------- POSIX ----------------------------

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

// --- basic Win32 typedefs -------------------------------------------------
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned short      WORD;
typedef unsigned int        UINT;
typedef unsigned int        DWORD;
typedef long                LONG;
typedef unsigned long       ULONG;
typedef int64_t             INT64;
typedef uint64_t            UINT64;
typedef int8_t              INT8;
typedef uint8_t             UINT8;
typedef int16_t             INT16;
typedef uint16_t            UINT16;
typedef int32_t             INT32;
typedef uint32_t            UINT32;
typedef void*               PVOID;
typedef void*               HANDLE;
typedef char*               LPSTR;
typedef const char*         LPCSTR;
typedef void*               LPVOID;

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define WINAPI
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

// --- case-insensitive compare --------------------------------------------
#define _stricmp  strcasecmp
#define _strnicmp strncasecmp

// --- interlocked operations ----------------------------------------------
// The modules rely on these for every parameter write; GCC/Clang __atomic
// builtins give the same sequential-consistency guarantee.
#define InterlockedExchange16(p, val)  (__atomic_exchange_n((short*)(p), (short)(val), __ATOMIC_SEQ_CST))
#define InterlockedExchange(p, val)    (__atomic_exchange_n((LONG*)(p), (LONG)(val), __ATOMIC_SEQ_CST))
#define InterlockedIncrement16(p)      (__atomic_add_fetch((short*)(p), 1, __ATOMIC_SEQ_CST))
#define InterlockedDecrement16(p)      (__atomic_sub_fetch((short*)(p), 1, __ATOMIC_SEQ_CST))
#define InterlockedIncrement(p)        (__atomic_add_fetch((LONG*)(p), 1, __ATOMIC_SEQ_CST))
#define InterlockedDecrement(p)        (__atomic_sub_fetch((LONG*)(p), 1, __ATOMIC_SEQ_CST))
#define InterlockedAnd16(p, val)       (__atomic_and_fetch((short*)(p), (short)(val), __ATOMIC_SEQ_CST))
#define InterlockedExchangeAdd(p, val) (__atomic_fetch_add((LONG*)(p), (LONG)(val), __ATOMIC_SEQ_CST))

// --- time -----------------------------------------------------------------
// GetTickCount()/timeGetTime() are milliseconds since an arbitrary origin, and
// every caller only ever takes differences, so CLOCK_MONOTONIC is an exact fit.
static inline DWORD platformTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((DWORD)ts.tv_sec * 1000u + (DWORD)(ts.tv_nsec / 1000000));
}
#define GetTickCount()  platformTickCount()
#define timeGetTime()   platformTickCount()

// Windows needs timeBeginPeriod to get a usable timer resolution; Linux
// nanosleep is already fine-grained, so these are deliberately no-ops.
#define timeBeginPeriod(x) ((void)(x))
#define timeEndPeriod(x)   ((void)(x))

static inline void platformSleepMs(DWORD ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#define Sleep(ms) platformSleepMs((DWORD)(ms))

#define GetLastError() ((DWORD)errno)

// --- critical sections ----------------------------------------------------
// Recursive so the semantics match Win32: report.cpp relies on being able to
// re-enter from reportSessionStart -> reportNoteEvent.
typedef struct {
    pthread_mutex_t m;
    int initialized;
} CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->m, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->initialized = 1;
}
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_lock(&cs->m); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { pthread_mutex_unlock(&cs->m); }
static inline void DeleteCriticalSection(CRITICAL_SECTION *cs) {
    if (cs->initialized) { pthread_mutex_destroy(&cs->m); cs->initialized = 0; }
}

// --- threads --------------------------------------------------------------
// Only the small subset clumsy uses: create, join with a timeout, close.
#define INFINITE        0xFFFFFFFFu
#define WAIT_OBJECT_0   0u
#define WAIT_TIMEOUT    258u
#define WAIT_ABANDONED  128u
#define WAIT_FAILED     0xFFFFFFFFu

typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);

HANDLE CreateThread(void *attrs, size_t stackSize, LPTHREAD_START_ROUTINE fn,
                    LPVOID arg, DWORD flags, DWORD *threadId);
DWORD  WaitForSingleObject(HANDLE h, DWORD timeoutMs);
DWORD  WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll,
                              DWORD timeoutMs);
BOOL   CloseHandle(HANDLE h);

// A mutex is modelled as a thread-less handle so divert.cpp's
// CreateMutex/WaitForSingleObject/ReleaseMutex trio needs no #ifdef.
HANDLE CreateMutex(void *attrs, BOOL initialOwner, const char *name);
BOOL   ReleaseMutex(HANDLE h);

// --- misc -----------------------------------------------------------------
#define GetCurrentProcessId() ((DWORD)getpid())

// Resolves the running executable via /proc/self/exe.
DWORD GetModuleFileNameA(void *module, char *buf, DWORD size);
#define GetModuleFileName GetModuleFileNameA

#define DebugBreak() ((void)0)

#endif  // _WIN32
