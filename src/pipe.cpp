// Named Pipe control API
//
// Listens on \\.\pipe\clumsy for JSON commands from an automation harness.
// Connection model: one JSON request → one JSON response → disconnect.
//
// Phase 2.9: this file is now only a transport. Every command is parsed and
// executed by controlDispatchJson() in controlapi.cpp, which the HTTP server
// shares, so the two can never drift apart. The wire protocol and the response
// shapes for the pre-Phase-2 commands (set / get_stats / stop) are unchanged,
// so existing automation scripts keep working without modification.
//
// Supported commands:
//   {"cmd":"set","module":"lag","enabled":true,"lag-time":100}
//   {"cmd":"get_stats"}
//   {"cmd":"stop"}                 — shuts clumsy down (legacy meaning)
//   {"cmd":"stop_capture"}         — stops capture, leaves clumsy running
//   {"cmd":"filter","filter":"udp and outbound","process":"game.exe"}
//   {"cmd":"get_modules"} {"cmd":"get_status"} {"cmd":"get_presets"}
//   {"cmd":"get_profiles"} {"cmd":"profile","name":"mobile-4g"}
//   {"cmd":"scenario","action":"load","path":"s.json"}
//   {"cmd":"pcap","action":"start","path":"out.pcap"}
//   {"cmd":"quit"}
//
// Responses always include "status":"ok" or "status":"error","message":"..."

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "common.h"
#include "controlapi.h"

// Named Pipes are a Windows IPC primitive with no direct POSIX equivalent, and
// the whole control surface is already reachable over HTTP on both platforms
// (controlapi.cpp is shared). Rather than invent a second, Linux-only IPC path
// that nothing uses, the pipe server is simply absent there.
#if defined(_WIN32)

#include <Windows.h>

#define PIPE_NAME     "\\\\.\\pipe\\clumsy"
#define PIPE_BUF_SIZE 8192

static HANDLE          pipeThread = NULL;
static volatile short  pipeStop   = 0;
volatile short         pipeStopRequested = 0;  // read by the main tick loop

// ---------------------------------------------------------------------------
// Pipe server thread
// ---------------------------------------------------------------------------

static DWORD WINAPI pipeServerLoop(LPVOID arg) {
    UNREFERENCED_PARAMETER(arg);

    while (!pipeStop) {
        HANDLE hPipe;
        char   reqBuf[PIPE_BUF_SIZE];
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
            std::string response;
            LOG("pipe recv: %s", reqBuf);
            response = controlDispatchJson(std::string(reqBuf, bytesRead));
            WriteFile(hPipe, response.c_str(), (DWORD)response.size(), &bytesWritten, NULL);
            LOG("pipe sent: %s", response.c_str());
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
        INFO("pipe: failed to start server thread (%lu)", GetLastError());
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

#else   // ------------------------- POSIX -------------------------

volatile short pipeStopRequested = 0;   // still read by the main tick loop

void pipeServerStart(void) {
    LOG("pipe: Named Pipe API is Windows-only; use the HTTP API instead.");
}

void pipeServerStop(void) {
}

#endif  // _WIN32
