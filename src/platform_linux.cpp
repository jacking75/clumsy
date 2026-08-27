// POSIX side of the platform compatibility layer  (Phase 4.5)
//
// Implements the handful of Win32 handle/thread primitives declared in
// platform.h. Kept deliberately small: only the exact semantics clumsy relies
// on, not a general Win32 emulation.

#include "platform.h"

#ifndef _WIN32

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {

enum HandleKind { HANDLE_THREAD, HANDLE_MUTEX };

struct PlatformHandle {
    HandleKind kind;
    // thread
    pthread_t  thread;
    LPTHREAD_START_ROUTINE fn;
    LPVOID     arg;
    int        joined;
    volatile int finished;
    // mutex
    pthread_mutex_t mutex;
};

void* threadTrampoline(void *raw) {
    PlatformHandle *h = static_cast<PlatformHandle *>(raw);
    h->fn(h->arg);
    __atomic_store_n(&h->finished, 1, __ATOMIC_SEQ_CST);
    return nullptr;
}

// Win32 waits take a millisecond timeout; pthread_join does not, so poll the
// finished flag. The wait paths here run at shutdown only, so a 2ms poll is
// cheaper than dragging in pthread_timedjoin_np and its portability caveats.
DWORD waitForThread(PlatformHandle *h, DWORD timeoutMs) {
    const DWORD start = platformTickCount();
    for (;;) {
        if (__atomic_load_n(&h->finished, __ATOMIC_SEQ_CST)) {
            if (!h->joined) {
                pthread_join(h->thread, nullptr);
                h->joined = 1;
            }
            return WAIT_OBJECT_0;
        }
        if (timeoutMs != INFINITE && (platformTickCount() - start) >= timeoutMs) {
            return WAIT_TIMEOUT;
        }
        platformSleepMs(2);
    }
}

} // namespace

HANDLE CreateThread(void *attrs, size_t stackSize, LPTHREAD_START_ROUTINE fn,
                    LPVOID arg, DWORD flags, DWORD *threadId) {
    UNREFERENCED_PARAMETER(attrs);
    UNREFERENCED_PARAMETER(flags);

    PlatformHandle *h = (PlatformHandle *)calloc(1, sizeof(PlatformHandle));
    if (!h) return nullptr;

    h->kind = HANDLE_THREAD;
    h->fn   = fn;
    h->arg  = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Win32 treats a small stackSize as a hint; honour it only when it is large
    // enough to be legal, otherwise take the system default.
    if (stackSize >= (size_t)PTHREAD_STACK_MIN) {
        pthread_attr_setstacksize(&attr, stackSize);
    }
    const int rc = pthread_create(&h->thread, &attr, threadTrampoline, h);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        free(h);
        errno = rc;
        return nullptr;
    }
    if (threadId) *threadId = 0;
    return h;
}

DWORD WaitForSingleObject(HANDLE handle, DWORD timeoutMs) {
    PlatformHandle *h = static_cast<PlatformHandle *>(handle);
    if (!h) return WAIT_FAILED;

    if (h->kind == HANDLE_MUTEX) {
        if (timeoutMs == INFINITE) {
            return pthread_mutex_lock(&h->mutex) == 0 ? WAIT_OBJECT_0 : WAIT_FAILED;
        }
        // Bounded acquire: try, then back off. divert.cpp uses a 40ms timeout on
        // the clock loop and treats WAIT_TIMEOUT as "skip this run", so a short
        // spin matches the intent without needing pthread_mutex_timedlock.
        const DWORD start = platformTickCount();
        for (;;) {
            if (pthread_mutex_trylock(&h->mutex) == 0) return WAIT_OBJECT_0;
            if ((platformTickCount() - start) >= timeoutMs) return WAIT_TIMEOUT;
            platformSleepMs(1);
        }
    }
    return waitForThread(h, timeoutMs);
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll,
                             DWORD timeoutMs) {
    // clumsy only ever calls this as (2, threads, TRUE, INFINITE).
    UNREFERENCED_PARAMETER(waitAll);
    for (DWORD i = 0; i < count; ++i) {
        const DWORD rc = WaitForSingleObject(handles[i], timeoutMs);
        if (rc != WAIT_OBJECT_0) return rc;
    }
    return WAIT_OBJECT_0;
}

BOOL CloseHandle(HANDLE handle) {
    PlatformHandle *h = static_cast<PlatformHandle *>(handle);
    if (!h) return FALSE;

    if (h->kind == HANDLE_MUTEX) {
        pthread_mutex_destroy(&h->mutex);
    } else if (!h->joined) {
        // Detach rather than leak the pthread when the caller never waited.
        pthread_detach(h->thread);
    }
    free(h);
    return TRUE;
}

HANDLE CreateMutex(void *attrs, BOOL initialOwner, const char *name) {
    UNREFERENCED_PARAMETER(attrs);
    UNREFERENCED_PARAMETER(name);

    PlatformHandle *h = (PlatformHandle *)calloc(1, sizeof(PlatformHandle));
    if (!h) return nullptr;
    h->kind = HANDLE_MUTEX;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&h->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (initialOwner) pthread_mutex_lock(&h->mutex);
    return h;
}

BOOL ReleaseMutex(HANDLE handle) {
    PlatformHandle *h = static_cast<PlatformHandle *>(handle);
    if (!h || h->kind != HANDLE_MUTEX) return FALSE;
    return pthread_mutex_unlock(&h->mutex) == 0 ? TRUE : FALSE;
}

DWORD GetModuleFileNameA(void *module, char *buf, DWORD size) {
    UNREFERENCED_PARAMETER(module);
    if (!buf || size == 0) return 0;
    const ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) {
        buf[0] = '\0';
        return 0;
    }
    buf[n] = '\0';
    return (DWORD)n;
}

#endif // !_WIN32
