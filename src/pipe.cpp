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

// Both platforms expose the same request/response protocol; only the transport
// primitive differs. Windows uses a Named Pipe, POSIX a Unix domain socket at
// /run/clumsy.sock. Existing Named Pipe automation ports by swapping the
// connect call - the JSON on the wire is identical, because both sides call
// straight into controlDispatchJson().
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

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_BUF_SIZE 8192
// /run is tmpfs on any modern distro, so a stale socket never survives a reboot.
// Falls back to /tmp when clumsy is running unprivileged.
#define SOCKET_PATH_PRIMARY  "/run/clumsy.sock"
#define SOCKET_PATH_FALLBACK "/tmp/clumsy.sock"

static int             listenFd = -1;
static char            socketPath[128] = "";
static HANDLE          pipeThread = NULL;
static volatile short  pipeStop   = 0;
volatile short         pipeStopRequested = 0;  // read by the main tick loop

static DWORD socketServerLoop(LPVOID arg) {
    UNREFERENCED_PARAMETER(arg);

    while (!pipeStop) {
        struct pollfd pfd;
        int clientFd;
        char reqBuf[SOCKET_BUF_SIZE];
        ssize_t got;

        // Poll instead of blocking in accept() so pipeServerStop() does not need
        // the dummy-connection trick the Windows side uses.
        pfd.fd = listenFd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 200) <= 0) continue;
        if (pipeStop) break;

        clientFd = accept(listenFd, NULL, NULL);
        if (clientFd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            LOG("control socket: accept failed (%s)", strerror(errno));
            continue;
        }

        memset(reqBuf, 0, sizeof(reqBuf));
        got = recv(clientFd, reqBuf, sizeof(reqBuf) - 1, 0);
        if (got > 0) {
            std::string response;
            LOG("control socket recv: %s", reqBuf);
            response = controlDispatchJson(std::string(reqBuf, (size_t)got));
            // Best effort: a client that hung up mid-exchange is not an error
            // worth reporting, the command already ran.
            if (send(clientFd, response.c_str(), response.size(), MSG_NOSIGNAL) < 0) {
                LOG("control socket: client went away before the reply");
            }
            LOG("control socket sent: %s", response.c_str());
        }

        close(clientFd);
    }
    return 0;
}

// Binds the listening socket, preferring /run and falling back to /tmp.
static int bindControlSocket(void) {
    static const char *candidates[] = { SOCKET_PATH_PRIMARY, SOCKET_PATH_FALLBACK };

    for (int i = 0; i < 2; ++i) {
        struct sockaddr_un addr;
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", candidates[i]);

        // A leftover socket file from a killed process would block bind().
        // Safe to remove: the single-instance lock in main.cpp already
        // guarantees no other clumsy is running.
        unlink(candidates[i]);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
            listen(fd, 4) == 0) {
            // clumsy usually runs as root; 0666 lets an unprivileged automation
            // script drive it, matching the Named Pipe's default reachability.
            chmod(candidates[i], 0666);
            snprintf(socketPath, sizeof(socketPath), "%s", candidates[i]);
            return fd;
        }
        close(fd);
    }
    return -1;
}

void pipeServerStart(void) {
    pipeStop          = 0;
    pipeStopRequested = 0;

    listenFd = bindControlSocket();
    if (listenFd < 0) {
        INFO("control socket: cannot bind %s or %s (%s); use the HTTP API instead.",
             SOCKET_PATH_PRIMARY, SOCKET_PATH_FALLBACK, strerror(errno));
        return;
    }

    pipeThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)socketServerLoop,
                              NULL, 0, NULL);
    if (pipeThread == NULL) {
        INFO("control socket: failed to start server thread");
        close(listenFd);
        listenFd = -1;
        unlink(socketPath);
        socketPath[0] = '\0';
        return;
    }
    LOG("control socket: server started on %s", socketPath);
}

void pipeServerStop(void) {
    if (!pipeThread) {
        if (listenFd >= 0) { close(listenFd); listenFd = -1; }
        if (socketPath[0]) { unlink(socketPath); socketPath[0] = '\0'; }
        return;
    }

    pipeStop = 1;
    WaitForSingleObject(pipeThread, 3000);
    CloseHandle(pipeThread);
    pipeThread = NULL;

    if (listenFd >= 0) { close(listenFd); listenFd = -1; }
    if (socketPath[0]) { unlink(socketPath); socketPath[0] = '\0'; }
    LOG("control socket: server stopped");
}

// Exposed so the startup banner can print the actual path in use.
const char* pipeServerPath(void) { return socketPath; }

#endif  // _WIN32
