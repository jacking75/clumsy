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
//   {"cmd":"replay","action":"start","path":"in.pcap"}
//   {"cmd":"metrics"}              — Prometheus text, inside "metrics"
//   {"cmd":"quit"}
//
// Responses always include "status":"ok" or "status":"error","message":"...".
// That holds for every command without exception: "metrics" returns the
// Prometheus exposition text as a JSON string field rather than raw, so a
// client can parse every reply the same way. Scrapers want GET /metrics.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "common.h"
#include "controlapi.h"

// A scenario or profile body is easily larger than one read buffer, so the
// request is accumulated rather than truncated at the buffer boundary. The cap
// matches the HTTP server's REQUEST_MAX_BYTES so both transports refuse the
// same thing, and going over it produces a real error reply instead of a
// silently truncated command or a dropped connection.
#define CONTROL_MAX_REQUEST 262144

// How much a refused request may still be read and thrown away. A message-mode
// pipe has to be drained to its end before the server answers: replying while
// the client is still writing leaves FlushFileBuffers() waiting for a client
// that is itself waiting for the pipe buffer to drain, and the server thread
// never gets back to accepting connections. Past this much, the connection is
// dropped without an answer instead.
#define CONTROL_MAX_DRAIN (8 * 1024 * 1024)

// The one reply that cannot come from controlDispatchJson(), because the
// request never got far enough to be parsed.
static const char *TOO_LARGE_JSON =
    "{\"status\":\"error\",\"message\":\"request exceeds the "
    STR(CONTROL_MAX_REQUEST) " byte limit\"}";

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

        // Read the JSON request. A message larger than reqBuf comes back as
        // ERROR_MORE_DATA with the buffer full, which the old code treated the
        // same as "nothing to read" - it hung up without a reply and the
        // client saw a broken pipe instead of an explanation.
        {
            std::string request, response;
            unsigned long long drained = 0;
            int tooLarge = 0, cutOff = 0;

            for (;;) {
                bytesRead = 0;
                ok = ReadFile(hPipe, reqBuf, sizeof(reqBuf), &bytesRead, NULL);
                drained += bytesRead;
                if (bytesRead > 0 && !tooLarge) {
                    request.append(reqBuf, bytesRead);
                    if (request.size() > (size_t)CONTROL_MAX_REQUEST) {
                        // Over the limit: stop storing, but keep reading (see
                        // CONTROL_MAX_DRAIN) so the client can finish its write
                        // and then read the refusal.
                        tooLarge = 1;
                        request.clear();
                    }
                }
                if (ok) break;                                  // whole message read
                if (GetLastError() != ERROR_MORE_DATA) break;   // a real failure
                if (drained > CONTROL_MAX_DRAIN) { cutOff = 1; break; }
            }

            if (cutOff) {
                INFO("pipe: dropping a request that exceeded %d bytes without "
                     "ending.", CONTROL_MAX_DRAIN);
            } else if (tooLarge) {
                INFO("pipe: refusing a request of %llu bytes (limit %d).",
                     drained, CONTROL_MAX_REQUEST);
                response = TOO_LARGE_JSON;
            } else if (!request.empty()) {
                LOG("pipe recv: %s", request.c_str());
                response = controlDispatchJson(request);
            }

            if (!response.empty()) {
                bytesWritten = 0;
                WriteFile(hPipe, response.c_str(), (DWORD)response.size(),
                          &bytesWritten, NULL);
                LOG("pipe sent: %s", response.c_str());
            }
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
// Bounds one client's turn on the single-threaded accept loop.
#define CONTROL_RECV_TIMEOUT_MS 15000

// Writes the whole reply. send() is free to accept only part of it - a signal
// during the call is enough - and stopping at the first short write would hand
// the client a truncated JSON object.
static int sendAllBytes(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

// Whether the accumulated text is a complete JSON value: braces and brackets
// balanced, quotes and escapes accounted for.
//
// Windows message-mode pipes frame the request for us; a Unix stream socket
// does not, and a client that sends its request and then waits for the answer
// never closes its end. Without this the read loop would sit on the receive
// timeout for every single command.
static int jsonLooksComplete(const std::string &text) {
    int  depth = 0;
    bool inStr = false, esc = false, sawStructure = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inStr) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; sawStructure = true; continue; }
        if (c == '{' || c == '[') { ++depth; sawStructure = true; continue; }
        if (c == '}' || c == ']') {
            if (--depth <= 0) return sawStructure;
        }
    }
    return 0;
}
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

        // This loop is the only thing serving the control socket, so a client
        // that connects and then says nothing must not be able to wedge it.
        {
            struct timeval tv;
            tv.tv_sec  = CONTROL_RECV_TIMEOUT_MS / 1000;
            tv.tv_usec = (CONTROL_RECV_TIMEOUT_MS % 1000) * 1000;
            setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        {
            std::string request, response;

            // A stream socket splits and coalesces freely, so one recv() is not
            // one request: keep reading until the JSON is balanced. The old
            // single read silently dropped everything past 8191 bytes and then
            // failed to parse what was left, reporting "malformed JSON" for a
            // request that was perfectly well formed.
            unsigned long long drained = 0;
            int tooLarge = 0;

            for (;;) {
                got = recv(clientFd, reqBuf, sizeof(reqBuf), 0);
                if (got <= 0) break;
                drained += (unsigned long long)got;
                if (!tooLarge) {
                    request.append(reqBuf, (size_t)got);
                    if (request.size() > (size_t)CONTROL_MAX_REQUEST) {
                        // Keep reading rather than answering into a socket the
                        // client is still filling; SO_RCVTIMEO bounds the wait.
                        tooLarge = 1;
                        request.clear();
                    }
                } else if (drained > CONTROL_MAX_DRAIN) {
                    break;
                }
                if (!tooLarge && jsonLooksComplete(request)) break;
            }

            if (tooLarge) {
                INFO("control socket: refusing a request of %llu bytes (limit %d).",
                     drained, CONTROL_MAX_REQUEST);
                response = TOO_LARGE_JSON;
            } else if (!request.empty()) {
                LOG("control socket recv: %s", request.c_str());
                response = controlDispatchJson(request);
            }

            // Best effort: a client that hung up mid-exchange is not an error
            // worth reporting, the command already ran.
            if (!response.empty()) {
                if (!sendAllBytes(clientFd, response.c_str(), response.size())) {
                    LOG("control socket: client went away before the reply");
                }
                LOG("control socket sent: %s", response.c_str());
            }
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
