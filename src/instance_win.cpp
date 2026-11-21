// Instance registry: Windows half  (multi-instance)
//
// Records live in %ProgramData%\clumsy\instances\<pid>.txt. Each is opened with
// FILE_FLAG_DELETE_ON_CLOSE and kept open while the instance captures, so the
// file disappears the moment the process ends - however it ends. A record that
// exists is therefore a live instance, and a crash leaves nothing behind.
//
// The check-then-record step is serialized across processes by a named mutex.
// If its holder dies, the next waiter gets WAIT_ABANDONED and simply proceeds.

#include <stdio.h>
#include <string.h>

#include "common.h"

namespace {

HANDLE registryMutex = NULL;
HANDLE ownRecord     = INVALID_HANDLE_VALUE;
char   registryDir[MAX_PATH] = "";

int ensureDir(char *errBuf, int errSize) {
    char base[MAX_PATH];
    DWORD n;

    if (registryDir[0]) return 1;
    n = GetEnvironmentVariableA("ProgramData", base, sizeof(base));
    if (n == 0 || n >= sizeof(base)) snprintf(base, sizeof(base), "C:\\ProgramData");

    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\clumsy", base);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, sizeof(dir), "%s\\clumsy\\instances", base);
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        snprintf(errBuf, (size_t)errSize,
                 "cannot create the instance registry %s (error %lu)", dir, GetLastError());
        return 0;
    }
    snprintf(registryDir, sizeof(registryDir), "%s", dir);
    return 1;
}

} // namespace

int regLock(char *errBuf, int errSize) {
    if (!ensureDir(errBuf, errSize)) return 0;
    if (!registryMutex) {
        registryMutex = CreateMutexA(NULL, FALSE, "Global\\clumsy-instance-registry");
        if (!registryMutex) {
            snprintf(errBuf, (size_t)errSize,
                     "cannot open the instance registry lock (error %lu)", GetLastError());
            return 0;
        }
    }
    // The holder only reads a few small files, so this is never long; the
    // timeout is there for a holder that hangs without dying.
    const DWORD w = WaitForSingleObject(registryMutex, 10000);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
        snprintf(errBuf, (size_t)errSize,
                 "timed out waiting for the instance registry lock held by another clumsy");
        return 0;
    }
    return 1;
}

void regUnlock(void) {
    if (registryMutex) ReleaseMutex(registryMutex);
}

void regForEachLive(void (*fn)(const char *text, void *ctx), void *ctx) {
    char pattern[MAX_PATH], own[64];
    WIN32_FIND_DATAA fd;

    snprintf(pattern, sizeof(pattern), "%s\\*.txt", registryDir);
    snprintf(own, sizeof(own), "%lu.txt", (unsigned long)GetCurrentProcessId());

    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH], text[FILTER_BUFSIZE * 6];
        DWORD got = 0;

        if (_stricmp(fd.cFileName, own) == 0) continue;
        snprintf(path, sizeof(path), "%s\\%s", registryDir, fd.cFileName);
        // The owner holds write and delete access, so both must be shared.
        // Failing to open means the owner has just gone (delete pending).
        HANDLE h = CreateFileA(path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (ReadFile(h, text, sizeof(text) - 1, &got, NULL) && got > 0) {
            text[got] = '\0';
            fn(text, ctx);
        }
        CloseHandle(h);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
}

int regWriteOwn(const char *text, char *errBuf, int errSize) {
    char path[MAX_PATH];
    DWORD written = 0;

    regRemoveOwn();
    snprintf(path, sizeof(path), "%s\\%lu.txt", registryDir, (unsigned long)GetCurrentProcessId());
    ownRecord = CreateFileA(path, GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (ownRecord == INVALID_HANDLE_VALUE) {
        snprintf(errBuf, (size_t)errSize,
                 "cannot write the instance record %s (error %lu)", path, GetLastError());
        return 0;
    }
    if (!WriteFile(ownRecord, text, (DWORD)strlen(text), &written, NULL) ||
        written != (DWORD)strlen(text)) {
        snprintf(errBuf, (size_t)errSize,
                 "cannot write the instance record %s (error %lu)", path, GetLastError());
        regRemoveOwn();
        return 0;
    }
    return 1;
}

void regRemoveOwn(void) {
    if (ownRecord != INVALID_HANDLE_VALUE) {
        CloseHandle(ownRecord);      // FILE_FLAG_DELETE_ON_CLOSE removes it
        ownRecord = INVALID_HANDLE_VALUE;
    }
}
