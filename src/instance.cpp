// Instance registry: the platform-neutral half  (multi-instance)
//
// Each clumsy that is capturing keeps one small text record in a shared
// directory (instance_win.cpp / instance_linux.cpp decide where and how it
// stays tied to the process). A record looks like:
//
//   pid=1234
//   web=http://127.0.0.1:8081/
//   pipe=\\.\pipe\client2
//   filter=(udp.SrcPort == 50002 and udp.DstPort == 7777) or ...
//   match=(...)            <- what is really captured, for filterOverlap()
//
// "match" is the filter plus the --process port list, and on Linux with
// --auto-iptables the derived NFQUEUE rules instead: those rules are wider
// than the filter, and a packet the first instance's rule takes never reaches
// the second instance's queue.
//
// The rest of the record exists for the error message: an agent told "your
// filter overlaps pid 1234 on port 8081" can go and stop that one.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "common.h"
#include "filterexpr.h"
#include "httpserver.h"

namespace {

int claimed = 0;

// Record values are one line each; a filter that arrived in a JSON body may
// contain newlines, which are whitespace to the filter language anyway.
std::string oneLine(const char *s) {
    std::string r = s ? s : "";
    for (char &c : r) if (c == '\r' || c == '\n') c = ' ';
    return r;
}

std::string field(const char *text, const char *key) {
    const size_t keyLen = strlen(key);
    const char *line = text;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        if (strncmp(line, key, keyLen) == 0 && line[keyLen] == '=') {
            const char *v = line + keyLen + 1;
            return end ? std::string(v, (size_t)(end - v)) : std::string(v);
        }
        line = end ? end + 1 : NULL;
    }
    return "";
}

std::string shorten(const std::string &s, size_t max) {
    return s.size() <= max ? s : s.substr(0, max) + "...";
}

struct CheckCtx {
    const char *matchExpr;
    int         result;        // 1 ok, CAPTURE_CONFLICT
    std::string message;
};

void checkOne(const char *text, void *arg) {
    CheckCtx *ctx = (CheckCtx*)arg;
    char detail[512];
    if (ctx->result != 1) return;          // the first conflict is enough

    const std::string match = field(text, "match");
    const int verdict = filterOverlap(ctx->matchExpr, match.c_str(), detail, sizeof(detail));
    if (verdict == FILTER_DISJOINT) return;

    const std::string pid  = field(text, "pid");
    const std::string web  = field(text, "web");
    const std::string pipe = field(text, "pipe");
    const std::string who  = "the clumsy with pid " + pid +
                             " (web " + (web.empty() ? "off" : web) +
                             ", pipe " + (pipe.empty() ? "none" : pipe) +
                             ") capturing \"" + shorten(field(text, "filter"), 160) + "\"";
    ctx->result = CAPTURE_CONFLICT;
    if (verdict == FILTER_OVERLAP) {
        ctx->message = "filter overlaps " + who + ": both would match " + detail +
                       ". Narrow one filter so no packet matches both, e.g. one client "
                       "per instance: (udp.SrcPort == <client> and udp.DstPort == <server>) "
                       "or (udp.SrcPort == <server> and udp.DstPort == <client>)";
    } else {
        ctx->message = "cannot verify that the filter does not overlap " + who + " (" +
                       detail + "). Simplify the filter";
    }
}

std::string webAddress(void) {
    const char *web = argGet("web");
    if (web && !parseBoolValue(web)) return "";
    return httpServerUrl();
}

} // namespace

int instanceClaim(const char *matchExpr, const char *filter, char *errBuf, int errSize) {
    CheckCtx ctx;
    char text[FILTER_BUFSIZE * 6];

    if (claimed) instanceRelease();
    if (!regLock(errBuf, errSize)) return 0;

    ctx.matchExpr = matchExpr;
    ctx.result    = 1;
    regForEachLive(checkOne, &ctx);
    if (ctx.result != 1) {
        regUnlock();
        snprintf(errBuf, (size_t)errSize, "%s", ctx.message.c_str());
        return ctx.result;
    }

    snprintf(text, sizeof(text), "pid=%lu\nweb=%s\npipe=%s\nfilter=%s\nmatch=%s\n",
             (unsigned long)GetCurrentProcessId(), webAddress().c_str(), pipeServerPath(),
             oneLine(filter).c_str(), oneLine(matchExpr).c_str());
    const int ok = regWriteOwn(text, errBuf, errSize);
    regUnlock();
    claimed = ok;
    return ok;
}

void instanceRelease(void) {
    if (!claimed) return;
    regRemoveOwn();
    claimed = 0;
}
