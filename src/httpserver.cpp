// Embedded HTTP server  (Phase 2.4 / 2.6 / 2.8).
//
// Deliberately narrow scope, per the roadmap's "direct implementation is lower
// risk than vendoring" rule:
//   * HTTP/1.1 GET and POST only
//   * no keep-alive — one request per connection, then close
//   * no chunked request bodies, no TLS, no compression
//
// That is enough for a single-operator local dashboard, and it keeps the whole
// server in one file the team can maintain forever.
//
// Threading: one accept loop thread, then one worker thread per connection so
// that a long-lived Server-Sent Events stream cannot block ordinary requests.
// Concurrency is capped by MAX_CONNECTIONS.

// Sockets are the one place the two platforms genuinely differ in spelling
// rather than in concept, so a thin shim covers it instead of forking the file.
#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <Windows.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
#  define closesocket(s) close(s)
#  define SD_BOTH        SHUT_RDWR
#  define WSAGetLastError() errno
// Winsock needs explicit startup/teardown; BSD sockets do not.
#  define WSACleanup()   ((void)0)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "common.h"
#include "controlapi.h"
#include "httpserver.h"
#include "json.h"

#define MAX_CONNECTIONS      32
#define REQUEST_MAX_BYTES    (256 * 1024)
#define RECV_TIMEOUT_MS      15000
#define WEB_ROOT_DIR         "web"
#define REPORT_BUF_SIZE      (512 * 1024)

static SOCKET          listenSocket = INVALID_SOCKET;
static HANDLE          acceptThread = NULL;
static volatile LONG   activeConnections = 0;
static volatile short  serverStop = 0;
static char            serverUrl[MSG_BUFSIZE]  = "";
static char            serverToken[128]        = "";
static short           authRequired = 0;
static char            webRootPath[MSG_BUFSIZE] = "";

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static int isLoopbackAddress(const char *addr) {
    if (!addr) return 1;
    return (strcmp(addr, "127.0.0.1") == 0 ||
            strcmp(addr, "localhost") == 0 ||
            strcmp(addr, "::1") == 0);
}

static void generateToken(char *out, int outSize) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    // Not cryptographic entropy, but this token only gates a LAN test tool and
    // is printed on the console for the operator to copy.
    unsigned int seed = (unsigned int)(GetTickCount() ^ GetCurrentProcessId());
    int i, n = outSize - 1;
    if (n > 24) n = 24;
    for (i = 0; i < n; ++i) {
        seed = seed * 1103515245u + 12345u;
        out[i] = alphabet[(seed >> 16) % (sizeof(alphabet) - 1)];
    }
    out[n] = '\0';
}

// Resolve "<exe dir>/web" once at startup.
static void buildWebRoot(void) {
    char *p;
    GetModuleFileNameA(NULL, webRootPath, MSG_BUFSIZE);
    p = strrchr(webRootPath, '\\');
    if (!p) p = strrchr(webRootPath, '/');
    if (p) strcpy(p + 1, WEB_ROOT_DIR);
    else   strcpy(webRootPath, WEB_ROOT_DIR);
}

#if !defined(_WIN32)
#include <csignal>
#endif

static int sendAll(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static const char* statusText(int code) {
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "OK";
    }
}

static void sendResponse(SOCKET s, int code, const char *contentType,
                         const char *body, int bodyLen,
                         const char *extraHeaders) {
    char header[1024];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        code, statusText(code), contentType, bodyLen,
        extraHeaders ? extraHeaders : "");
    if (n <= 0) return;
    if (!sendAll(s, header, n)) return;
    if (bodyLen > 0) sendAll(s, body, bodyLen);
}

static void sendJson(SOCKET s, int code, const std::string &json) {
    sendResponse(s, code, "application/json; charset=utf-8",
                 json.c_str(), (int)json.size(), NULL);
}

static void sendText(SOCKET s, int code, const char *text) {
    sendResponse(s, code, "text/plain; charset=utf-8", text, (int)strlen(text), NULL);
}

// ---------------------------------------------------------------------------
// Request parsing
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;         // without query string
    std::string query;
    std::string body;
    std::string token;        // from X-Clumsy-Token or ?token=
};

static std::string urlDecode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = { in[i+1], in[i+2], '\0' };
            char *stop = NULL;
            long v = strtol(hex, &stop, 16);
            if (stop && *stop == '\0') {
                out.push_back((char)v);
                i += 2;
                continue;
            }
        }
        if (in[i] == '+') out.push_back(' ');
        else              out.push_back(in[i]);
    }
    return out;
}

static std::string queryParam(const std::string &query, const std::string &key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return urlDecode(pair.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

// Reads a full request: request line, headers, and Content-Length bytes of body.
static int readRequest(SOCKET s, HttpRequest *req) {
    std::string buf;
    char chunk[4096];
    size_t headerEnd = std::string::npos;
    long contentLength = 0;

    // 1. read until the header terminator
    for (;;) {
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) return 0;
        buf.append(chunk, n);
        headerEnd = buf.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (buf.size() > 32 * 1024) return 0; // header flood guard
    }

    std::string head = buf.substr(0, headerEnd);

    // 2. request line
    size_t lineEnd = head.find("\r\n");
    std::string requestLine = head.substr(0, lineEnd == std::string::npos ? head.size() : lineEnd);
    size_t sp1 = requestLine.find(' ');
    if (sp1 == std::string::npos) return 0;
    size_t sp2 = requestLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return 0;
    req->method = requestLine.substr(0, sp1);
    std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t q = target.find('?');
    if (q == std::string::npos) {
        req->path = urlDecode(target);
    } else {
        req->path  = urlDecode(target.substr(0, q));
        req->query = target.substr(q + 1);
        req->token = queryParam(req->query, "token");
    }

    // 3. headers we care about
    size_t pos = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
            if (_stricmp(name.c_str(), "Content-Length") == 0) {
                contentLength = atol(value.c_str());
            } else if (_stricmp(name.c_str(), "X-Clumsy-Token") == 0) {
                req->token = value;
            }
        }
        if (e == std::string::npos) break;
        pos = e + 2;
    }

    if (contentLength < 0 || contentLength > REQUEST_MAX_BYTES) return -1;

    // 4. body
    req->body = buf.substr(headerEnd + 4);
    while ((long)req->body.size() < contentLength) {
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        req->body.append(chunk, n);
    }
    if ((long)req->body.size() > contentLength) req->body.resize(contentLength);

    return 1;
}

static int isAuthorized(const HttpRequest &req) {
    if (!authRequired) return 1;
    return req.token == serverToken;
}

// ---------------------------------------------------------------------------
// Static file serving
// ---------------------------------------------------------------------------

static const char* mimeForPath(const std::string &path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".js")   return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".png")  return "image/png";
    if (ext == ".ico")  return "image/x-icon";
    return "application/octet-stream";
}

// Rejects anything that could escape the web root.
static int isSafeRelativePath(const std::string &p) {
    if (p.empty()) return 0;
    if (p.find("..") != std::string::npos) return 0;
    if (p.find(':') != std::string::npos) return 0;
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/';
        if (!ok) return 0;
    }
    return 1;
}

static void serveStatic(SOCKET s, const std::string &path) {
    std::string rel = (path == "/") ? "index.html" : path.substr(1);
    if (!isSafeRelativePath(rel)) {
        sendText(s, 403, "Forbidden");
        return;
    }

#if defined(_WIN32)
    std::string full = std::string(webRootPath) + "\\" + rel;
    for (size_t i = 0; i < full.size(); ++i) if (full[i] == '/') full[i] = '\\';
#else
    std::string full = std::string(webRootPath) + "/" + rel;
#endif

    FILE *f = fopen(full.c_str(), "rb");
    if (!f) {
        if (rel == "index.html") {
            static const char *missing =
                "<!doctype html><meta charset=\"utf-8\"><title>clumsy</title>"
                "<body style=\"font:14px system-ui;padding:2rem\">"
                "<h1>Dashboard files missing</h1>"
                "<p>Could not find <code>web/index.html</code> next to clumsy.exe. "
                "Copy the <code>etc/web/</code> folder into the output directory, or "
                "rebuild so the post-build step copies it.</p>"
                "<p>The REST API is unaffected: try <a href=\"/api/docs\">/api/docs</a>.</p>";
            sendResponse(s, 200, "text/html; charset=utf-8", missing, (int)strlen(missing), NULL);
        } else {
            sendText(s, 404, "Not Found");
        }
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 8 * 1024 * 1024) { fclose(f); sendText(s, 500, "File too large"); return; }

    std::string content;
    content.resize((size_t)size);
    size_t got = size > 0 ? fread(&content[0], 1, (size_t)size, f) : 0;
    fclose(f);
    content.resize(got);

    sendResponse(s, 200, mimeForPath(rel), content.data(), (int)content.size(), NULL);
}

// ---------------------------------------------------------------------------
// Server-Sent Events  (Phase 2.6)
// ---------------------------------------------------------------------------

static void serveEventStream(SOCKET s) {
    static const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    if (!sendAll(s, header, (int)strlen(header))) return;

    while (!serverStop) {
        std::string payload = apiStatsJson();
        std::string frame = "event: stats\ndata: " + payload + "\n\n";
        if (!sendAll(s, frame.c_str(), (int)frame.size())) return; // client went away
        Sleep(CLOCK_TICK_MS);
    }
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

// Splits "/api/profiles/mobile-4g/apply" against "/api/profiles/" + "/apply".
static int matchWrapped(const std::string &path, const char *prefix,
                        const char *suffix, std::string *middle) {
    size_t plen = strlen(prefix), slen = strlen(suffix);
    if (path.size() <= plen + slen) return 0;
    if (path.compare(0, plen, prefix) != 0) return 0;
    if (path.compare(path.size() - slen, slen, suffix) != 0) return 0;
    *middle = path.substr(plen, path.size() - plen - slen);
    return !middle->empty();
}

static void serveReport(SOCKET s) {
    std::string buf;
    buf.resize(REPORT_BUF_SIZE);
    int n = reportRenderHtml(&buf[0], REPORT_BUF_SIZE);
    if (n <= 0) {
        sendText(s, 500, "Failed to render report");
        return;
    }
    sendResponse(s, 200, "text/html; charset=utf-8", buf.data(), n,
                 "Content-Disposition: attachment; filename=\"clumsy-report.html\"\r\n");
}

static void routeRequest(SOCKET s, const HttpRequest &req) {
    const std::string &path = req.path;
    int status = 200;
    std::string middle;

    // Health check is intentionally unauthenticated (Phase 3.4).
    if (path == "/api/health") { sendJson(s, 200, apiHealthJson()); return; }

    if (!isAuthorized(req)) {
        sendJson(s, 401, "{\"status\":\"error\",\"message\":\"missing or invalid token; "
                         "pass ?token=... or the X-Clumsy-Token header\"}");
        return;
    }

    if (req.method == "GET") {
        if (path == "/api/modules")  { sendJson(s, 200, apiModulesJson());  return; }
        if (path == "/api/stats")    { sendJson(s, 200, apiStatsJson());    return; }
        if (path == "/api/status")   { sendJson(s, 200, apiStatusJson());   return; }
        if (path == "/api/profiles") { sendJson(s, 200, apiProfilesJson()); return; }
        if (path == "/api/presets")  { sendJson(s, 200, apiPresetsJson());  return; }
        if (path == "/api/docs")     { sendJson(s, 200, apiDocsJson());     return; }
        if (path == "/api/report")   { serveReport(s);                      return; }
        if (path == "/api/stream")   { serveEventStream(s);                 return; }
        if (path.compare(0, 5, "/api/") == 0) { sendText(s, 404, "Unknown API endpoint"); return; }
        serveStatic(s, path);
        return;
    }

    if (req.method == "POST") {
        if (path == "/api/filter")          { sendJson(s, status, apiSetFilter(req.body, &status)); return; }
        if (path == "/api/stop")            { sendJson(s, status, apiStopCapture(&status));         return; }
        if (path == "/api/quit")            { sendJson(s, status, apiQuit(&status));                return; }
        if (path == "/api/profiles")        { sendJson(s, status, apiSaveProfile(req.body, &status)); return; }
        if (path == "/api/presets")         { sendJson(s, status, apiSavePreset(req.body, &status)); return; }
        if (path == "/api/scenario/load")   { sendJson(s, status, apiScenarioLoad(req.body, &status)); return; }
        if (path == "/api/scenario/start")  { sendJson(s, status, apiScenarioStart(&status));       return; }
        if (path == "/api/scenario/stop")   { sendJson(s, status, apiScenarioStop(&status));        return; }
        if (path == "/api/pcap/start")      { sendJson(s, status, apiPcapStart(req.body, &status)); return; }
        if (path == "/api/pcap/stop")       { sendJson(s, status, apiPcapStop(&status));            return; }
        if (matchWrapped(path, "/api/profiles/", "/apply", &middle)) {
            std::string resp = apiApplyProfile(middle, &status);
            sendJson(s, status, resp);
            return;
        }
        if (path.compare(0, 13, "/api/modules/") == 0) {
            std::string name = path.substr(13);
            std::string resp = apiSetModule(name, req.body, &status);
            sendJson(s, status, resp);
            return;
        }
        sendText(s, 404, "Unknown API endpoint");
        return;
    }

    sendText(s, 405, "Method Not Allowed");
}

// ---------------------------------------------------------------------------
// Connection and accept threads
// ---------------------------------------------------------------------------

static DWORD WINAPI connectionThread(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    HttpRequest req;

#if defined(_WIN32)
    DWORD timeout = RECV_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    // POSIX wants a timeval here, not a millisecond DWORD.
    struct timeval tv;
    tv.tv_sec  = RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    int rc = readRequest(s, &req);
    if (rc == 1) {
        LOG("http: %s %s", req.method.c_str(), req.path.c_str());
        routeRequest(s, req);
    } else if (rc == -1) {
        sendText(s, 413, "Payload Too Large");
    }

    shutdown(s, SD_BOTH);
    closesocket(s);
    InterlockedDecrement(&activeConnections);
    return 0;
}

static DWORD WINAPI acceptLoop(LPVOID arg) {
    UNREFERENCED_PARAMETER(arg);

    while (!serverStop) {
        SOCKET client = accept(listenSocket, NULL, NULL);
        if (client == INVALID_SOCKET) {
            if (serverStop) break;
            Sleep(50);
            continue;
        }
        if (activeConnections >= MAX_CONNECTIONS) {
            static const char *busy =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send(client, busy, (int)strlen(busy), 0);
            closesocket(client);
            continue;
        }
        InterlockedIncrement(&activeConnections);
        HANDLE h = CreateThread(NULL, 0, connectionThread,
                                (LPVOID)(uintptr_t)client, 0, NULL);
        if (h) {
            CloseHandle(h);
        } else {
            InterlockedDecrement(&activeConnections);
            closesocket(client);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int httpServerStart(const char *bindAddr, int port, const char *token) {
    struct sockaddr_in addr;
    int reuse = 1;

    if (port <= 0 || port > 65535) {
        INFO("web: invalid port %d", port);
        return 0;
    }

#if defined(_WIN32)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            INFO("web: WSAStartup failed (%d)", WSAGetLastError());
            return 0;
        }
    }
#else
    // A client that vanishes mid-response would otherwise kill the process with
    // SIGPIPE; send() reporting EPIPE is what the code already handles.
    signal(SIGPIPE, SIG_IGN);
#endif

    // --- authentication policy (Phase 2.8 / 3.4) ---
    // Loopback stays token-free for single-user convenience. Any other bind
    // address is reachable from the network, so a token becomes mandatory.
    serverToken[0] = '\0';
    authRequired = 0;
    if (token && token[0]) {
        strncpy(serverToken, token, sizeof(serverToken) - 1);
        serverToken[sizeof(serverToken) - 1] = '\0';
        authRequired = 1;
    } else if (!isLoopbackAddress(bindAddr)) {
        generateToken(serverToken, sizeof(serverToken));
        authRequired = 1;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        INFO("web: socket() failed (%d)", WSAGetLastError());
        WSACleanup();
        return 0;
    }
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    const char *resolved =
        (!bindAddr || !bindAddr[0] || strcmp(bindAddr, "localhost") == 0)
        ? "127.0.0.1" : bindAddr;
    if (inet_pton(AF_INET, resolved, &addr.sin_addr) != 1) {
        INFO("web: '%s' is not a valid IPv4 bind address", bindAddr);
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    if (bind(listenSocket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        INFO("web: cannot bind %s:%d (%d) - is another clumsy or service using it?",
             bindAddr ? bindAddr : "127.0.0.1", port, WSAGetLastError());
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        INFO("web: listen() failed (%d)", WSAGetLastError());
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    buildWebRoot();
    serverStop = 0;
    activeConnections = 0;

    acceptThread = CreateThread(NULL, 0, acceptLoop, NULL, 0, NULL);
    if (!acceptThread) {
        INFO("web: failed to create accept thread (%lu)", GetLastError());
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }

    {
        const char *shown = (!bindAddr || !bindAddr[0]) ? "127.0.0.1" : bindAddr;
        if (authRequired) {
            snprintf(serverUrl, sizeof(serverUrl), "http://%s:%d/?token=%s",
                     shown, port, serverToken);
        } else {
            snprintf(serverUrl, sizeof(serverUrl), "http://%s:%d/", shown, port);
        }
    }

    if (!isLoopbackAddress(bindAddr)) {
        INFO("");
        INFO("  *** SECURITY WARNING ***");
        INFO("  The dashboard is bound to %s, reachable from the network.", bindAddr);
        INFO("  Anyone who can reach this port AND has the token can degrade your");
        INFO("  network traffic. Use it only on a trusted, isolated test network.");
        INFO("");
    }

    return 1;
}

void httpServerStop(void) {
    if (listenSocket == INVALID_SOCKET && !acceptThread) return;

    InterlockedExchange16(&serverStop, 1);

    if (listenSocket != INVALID_SOCKET) {
        // closing the listener wakes the blocking accept()
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }
    if (acceptThread) {
        WaitForSingleObject(acceptThread, 3000);
        CloseHandle(acceptThread);
        acceptThread = NULL;
    }

    // Give in-flight connections (notably SSE streams, which poll serverStop
    // once per tick) a moment to notice and unwind.
    for (int i = 0; i < 40 && activeConnections > 0; ++i) Sleep(50);

    WSACleanup();
    LOG("web: server stopped");
}

const char* httpServerUrl(void)   { return serverUrl; }
const char* httpServerToken(void) { return serverToken; }
