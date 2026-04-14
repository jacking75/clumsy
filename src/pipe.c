// Named Pipe control API
//
// Listens on \\.\pipe\clumsy for JSON commands from an automation harness.
// Connection model: one JSON request  → one JSON response → disconnect.
//
// Supported commands:
//   {"cmd":"set","module":"lag","enabled":true,"lag-time":100}
//   {"cmd":"get_stats"}
//   {"cmd":"stop"}
//
// Responses always include "status":"ok" or "status":"error","message":"..."
//
// Thread safety: setParam writes to volatile module params (aligned 16/32-bit
// reads/writes are atomic on x86/x64). enabled flags use InterlockedExchange16.

#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

#define PIPE_NAME     "\\\\.\\pipe\\clumsy"
#define PIPE_BUF_SIZE 4096

static HANDLE          pipeThread = NULL;
static volatile short  pipeStop   = 0;
volatile short         pipeStopRequested = 0;  // read by main-thread timer

// ---------------------------------------------------------------------------
// Minimal flat-JSON helpers
// ---------------------------------------------------------------------------

// Extract the bare value for `key` from a flat JSON object.
// String values: surrounding quotes are removed.
// Number / boolean values: returned as-is.
// Returns 1 if found, 0 if not.
static int jsonGet(const char *json, const char *key, char *out, int outSize) {
    char needle[128];
    const char *p, *start, *end;
    int len;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    if (*p == '"') {
        start = p + 1;
        end   = strchr(start, '"');
        if (!end) return 0;
    } else {
        start = p;
        end   = p;
        while (*end && *end != ',' && *end != '}' &&
               *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') end++;
    }

    len = (int)(end - start);
    if (len >= outSize) len = outSize - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

// Iterate all top-level key:value pairs in a flat JSON object and call
// module->setParam for each one that is not a meta key (cmd/module/enabled).
static void applyModuleParams(const char *json, Module *mod) {
    const char *p;
    char key[64], val[128];

    if (!mod->setParam) return;

    p = strchr(json, '{');
    if (!p) return;
    p++;

    for (;;) {
        const char *keyEnd, *valStart, *valEnd;
        int klen, vlen;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') break;
        p++;

        keyEnd = strchr(p, '"');
        if (!keyEnd) break;
        klen = (int)(keyEnd - p);
        if (klen <= 0 || klen >= (int)sizeof(key)) { p = keyEnd + 1; continue; }
        memcpy(key, p, klen); key[klen] = '\0';
        p = keyEnd + 1;

        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;

        if (*p == '"') {
            p++;
            valEnd = strchr(p, '"');
            if (!valEnd) break;
            vlen = (int)(valEnd - p);
            if (vlen >= (int)sizeof(val)) vlen = (int)sizeof(val) - 1;
            memcpy(val, p, vlen); val[vlen] = '\0';
            p = valEnd + 1;
        } else {
            valStart = p;
            while (*p && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            vlen = (int)(p - valStart);
            if (vlen >= (int)sizeof(val)) vlen = (int)sizeof(val) - 1;
            memcpy(val, valStart, vlen); val[vlen] = '\0';
        }

        // skip meta keys; pass everything else to the module
        if (strcmp(key, "cmd")     == 0) continue;
        if (strcmp(key, "module")  == 0) continue;
        if (strcmp(key, "enabled") == 0) continue;

        mod->setParam(key, val);
    }
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void handleSet(const char *json, char *resp, int respSize) {
    char modName[32], val[64];
    Module *mod = NULL;
    int ix;

    if (!jsonGet(json, "module", modName, sizeof(modName))) {
        snprintf(resp, respSize,
            "{\"status\":\"error\",\"message\":\"missing module field\"}");
        return;
    }

    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (strcmp(modules[ix]->shortName, modName) == 0) {
            mod = modules[ix];
            break;
        }
    }
    if (!mod) {
        snprintf(resp, respSize,
            "{\"status\":\"error\",\"message\":\"unknown module: %s\"}", modName);
        return;
    }

    // apply enabled flag if present
    if (jsonGet(json, "enabled", val, sizeof(val))) {
        short en = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
        InterlockedExchange16(mod->enabledFlag, en);
        LOG("pipe: %s enabled=%d", modName, (int)en);
    }

    // apply any module-specific params
    applyModuleParams(json, mod);

    snprintf(resp, respSize, "{\"status\":\"ok\",\"module\":\"%s\"}", modName);
}

static void handleGetStats(char *resp, int respSize) {
    char buf[PIPE_BUF_SIZE];
    int ix, pos = 0;

    pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
        "{\"status\":\"ok\",\"modules\":{");

    for (ix = 0; ix < MODULE_CNT; ++ix) {
        Module *m = modules[ix];
        if (ix > 0 && pos < (int)sizeof(buf) - 1) buf[pos++] = ',';
        pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
            "\"%s\":{\"enabled\":%s,\"affected\":%ld}",
            m->shortName,
            (*m->enabledFlag) ? "true" : "false",
            m->affectedCount);
    }

    pos += snprintf(buf + pos, (int)sizeof(buf) - pos,
        "},\"captured\":%ld,\"sent\":%ld}",
        statsCapturedTotal, statsSentTotal);

    strncpy(resp, buf, respSize - 1);
    resp[respSize - 1] = '\0';
}

static void handleStop(char *resp, int respSize) {
    snprintf(resp, respSize,
        "{\"status\":\"ok\",\"message\":\"stopping\"}");
    InterlockedExchange16(&pipeStopRequested, 1);
    LOG("pipe: stop requested");
}

static void dispatchCommand(const char *json, char *resp, int respSize) {
    char cmd[32];

    if (!jsonGet(json, "cmd", cmd, sizeof(cmd))) {
        snprintf(resp, respSize,
            "{\"status\":\"error\",\"message\":\"missing cmd field\"}");
        return;
    }

    if      (strcmp(cmd, "set")       == 0) handleSet(json, resp, respSize);
    else if (strcmp(cmd, "get_stats") == 0) handleGetStats(resp, respSize);
    else if (strcmp(cmd, "stop")      == 0) handleStop(resp, respSize);
    else
        snprintf(resp, respSize,
            "{\"status\":\"error\",\"message\":\"unknown cmd: %s\"}", cmd);
}

// ---------------------------------------------------------------------------
// Pipe server thread
// ---------------------------------------------------------------------------

static DWORD WINAPI pipeServerLoop(LPVOID arg) {
    UNREFERENCED_PARAMETER(arg);

    while (!pipeStop) {
        HANDLE hPipe;
        char   reqBuf[PIPE_BUF_SIZE];
        char   resBuf[PIPE_BUF_SIZE];
        DWORD  bytesRead = 0, bytesWritten = 0;
        BOOL   ok;

        hPipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1,              // only one simultaneous instance
            PIPE_BUF_SIZE,
            PIPE_BUF_SIZE,
            0,              // default timeout
            NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            LOG("pipe: CreateNamedPipe failed (%lu)", GetLastError());
            Sleep(1000);
            continue;
        }

        // blocks until a client connects (or pipeStop wakes us via dummy connect)
        ok = ConnectNamedPipe(hPipe, NULL);
        if (!ok && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(hPipe);
            continue;
        }

        // read the JSON request
        memset(reqBuf, 0, sizeof(reqBuf));
        ok = ReadFile(hPipe, reqBuf, sizeof(reqBuf) - 1, &bytesRead, NULL);
        if (ok && bytesRead > 0) {
            LOG("pipe recv: %s", reqBuf);
            memset(resBuf, 0, sizeof(resBuf));
            dispatchCommand(reqBuf, resBuf, sizeof(resBuf));
            WriteFile(hPipe, resBuf, (DWORD)strlen(resBuf), &bytesWritten, NULL);
            LOG("pipe sent: %s", resBuf);
        }

        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void pipeServerStart(void) {
    pipeStop          = 0;
    pipeStopRequested = 0;
    pipeThread = CreateThread(NULL, 0, pipeServerLoop, NULL, 0, NULL);
    if (pipeThread == NULL) {
        LOG("pipe: failed to start server thread (%lu)", GetLastError());
    } else {
        LOG("pipe: server started on %s", PIPE_NAME);
    }
}

void pipeServerStop(void) {
    HANDLE hWakeup;
    if (!pipeThread) return;

    pipeStop = 1;
    // unblock the ConnectNamedPipe by making a brief dummy client connection
    hWakeup = CreateFileA(PIPE_NAME, GENERIC_READ, 0, NULL,
                          OPEN_EXISTING, 0, NULL);
    if (hWakeup != INVALID_HANDLE_VALUE) CloseHandle(hWakeup);

    WaitForSingleObject(pipeThread, 3000);
    CloseHandle(pipeThread);
    pipeThread = NULL;
    LOG("pipe: server stopped");
}
