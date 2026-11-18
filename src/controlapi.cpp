// Transport-independent control layer  (Phase 2.5).
//
// Thread safety: this file runs on the Named Pipe thread and on HTTP worker
// threads. It only ever touches module parameters (written with the
// Interlocked* helpers, exactly like the old UI callbacks did) and the
// app* control surface in main.cpp, which serializes itself. It never walks
// the packet list — that belongs to the divert threads alone.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "common.h"
#include "controlapi.h"
#include "json.h"

// ---------------------------------------------------------------------------
// Small serialization helpers
// ---------------------------------------------------------------------------

static std::string quoted(const std::string &s) {
    return "\"" + jsonEscape(s) + "\"";
}

static std::string numToStr(long v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", v);
    return buf;
}

static std::string dblToStr(double v) {
    char buf[48];
    // Range first, then the cast: converting an out-of-range double to
    // long long is undefined behaviour, and some of what reaches here is
    // straight from a request body (apiReplayStart echoes back the "speed"
    // field, which a client is free to send as 1e19 or as Infinity).
    if (v < 1e15 && v > -1e15 && v == (double)(long long)v) {
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
    } else {
        snprintf(buf, sizeof(buf), "%.4g", v);
    }
    return buf;
}

static std::string okJson(const std::string &extra = "") {
    if (extra.empty()) return "{\"status\":\"ok\"}";
    return "{\"status\":\"ok\"," + extra + "}";
}

static std::string errJson(const std::string &message) {
    return "{\"status\":\"error\",\"message\":" + quoted(message) + "}";
}

static Module* findModule(const std::string &shortName) {
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        if (shortName == modules[ix]->shortName) return modules[ix];
    }
    return nullptr;
}

// "lag, jitter, drop, ..." - appended to "unknown module" errors so the caller
// can correct itself from the reply instead of going to look the names up.
static std::string moduleNameList() {
    std::string out;
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        if (ix) out += ", ";
        out += modules[ix]->shortName;
    }
    return out;
}

static std::string moduleKeyList(const Module *m) {
    std::string out;
    for (int i = 0; i < m->paramSpecCount; ++i) {
        if (i) out += ",";
        out += m->paramSpecs[i].key;
    }
    return out;
}

// ,"warnings":["...","..."] - or nothing at all when the list is empty, so
// replies that had nothing to complain about keep their old shape.
static std::string warningsField(const std::vector<std::string> &warnings) {
    if (warnings.empty()) return "";
    std::string out = ",\"warnings\":[";
    for (size_t i = 0; i < warnings.size(); ++i) {
        if (i) out += ",";
        out += quoted(warnings[i]);
    }
    out += "]";
    return out;
}

static std::vector<std::string> splitLines(const char *text) {
    std::vector<std::string> out;
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        out.push_back(nl ? std::string(p, nl - p) : std::string(p));
        p = nl ? nl + 1 : nullptr;
    }
    return out;
}

// What scenarioLint() has to say about the scenario that was just loaded.
static std::string scenarioWarningsField() {
    char lint[4096];
    if (scenarioLint(lint, sizeof(lint)) == 0) return "";
    return warningsField(splitLines(lint));
}

// A module parameter map: {"lag-time":"50","lag-inbound":"true"}
static std::string moduleParamsJson(Module *m) {
    ParamKV kv[32];
    int n = m->getParams ? m->getParams(kv, 32) : 0;
    std::string out = "{";
    for (int i = 0; i < n; ++i) {
        if (i) out += ",";
        out += quoted(kv[i].key) + ":" + quoted(kv[i].val);
    }
    out += "}";
    return out;
}

static std::string paramSpecsJson(Module *m) {
    std::string out = "[";
    for (int i = 0; i < m->paramSpecCount; ++i) {
        const ParamSpec &s = m->paramSpecs[i];
        if (i) out += ",";
        out += "{\"key\":" + quoted(s.key) +
               ",\"label\":" + quoted(s.label) +
               ",\"type\":" + quoted(s.type) +
               ",\"min\":" + dblToStr(s.minVal) +
               ",\"max\":" + dblToStr(s.maxVal);
        // Enum parameters ship their choices so the dashboard can render a
        // dropdown without knowing which module it is looking at.
        if (s.options && s.options[0]) {
            out += ",\"options\":[";
            const char *p = s.options;
            bool firstOpt = true;
            while (*p) {
                const char *comma = strchr(p, ',');
                std::string one = comma ? std::string(p, comma - p) : std::string(p);
                if (!one.empty()) {
                    if (!firstOpt) out += ",";
                    out += quoted(one);
                    firstOpt = false;
                }
                if (!comma) break;
                p = comma + 1;
            }
            out += "]";
        }
        out += "}";
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------
// GET payloads
// ---------------------------------------------------------------------------

std::string apiModulesJson() {
    std::string out = "{\"status\":\"ok\",\"modules\":[";
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        Module *m = modules[ix];
        if (ix) out += ",";
        out += "{\"shortName\":" + quoted(m->shortName) +
               ",\"displayName\":" + quoted(m->displayName) +
               ",\"description\":" + quoted(moduleDescription(m->shortName)) +
               ",\"enabled\":" + std::string(*m->enabledFlag ? "true" : "false") +
               ",\"affected\":" + numToStr(m->affectedCount) +
               ",\"params\":" + moduleParamsJson(m) +
               ",\"paramSpecs\":" + paramSpecsJson(m) + "}";
    }
    out += "]}";
    return out;
}

std::string apiStatsJson() {
    std::string out = "{\"status\":\"ok\",\"capturing\":";
    out += appIsCapturing() ? "true" : "false";
    out += ",\"elapsedMs\":" + numToStr((long)appCaptureElapsedMs());
    out += ",\"captured\":" + numToStr(statsCapturedTotal);
    out += ",\"sent\":" + numToStr(statsSentTotal);
    out += ",\"buffers\":{\"lag\":" + numToStr(lagGetBufSize()) +
           ",\"jitter\":" + numToStr(jitterGetBufSize()) +
           ",\"bandwidth\":" + numToStr(bandwidthGetBufSize()) +
           ",\"bandwidthLimitKBps\":" + numToStr(bandwidthGetLimitKBps()) + "}";
    out += ",\"pcap\":{\"active\":" + std::string(pcapExportIsActive() ? "true" : "false") +
           ",\"packets\":" + numToStr(pcapExportCount()) +
           ",\"bytes\":" + numToStr(pcapExportBytes()) + "}";
    out += ",\"scenario\":{\"loaded\":" + std::string(scenarioIsLoaded() ? "true" : "false") +
           ",\"active\":" + std::string(scenarioIsActive() ? "true" : "false") +
           ",\"steps\":" + numToStr(scenarioStepCount()) + "}";
    out += ",\"replay\":{\"active\":" + std::string(pcapReplayIsActive() ? "true" : "false") +
           ",\"read\":" + numToStr(pcapReplayRead()) +
           ",\"sent\":" + numToStr(pcapReplaySent()) +
           ",\"failed\":" + numToStr(pcapReplayFailed()) + "}";
    // Percentiles are read off the histogram, so this costs the same whether
    // the session delayed ten packets or ten million.
    out += ",\"latency\":{\"count\":" + numToStr(latencyCount()) +
           ",\"min\":" + numToStr(latencyMin()) +
           ",\"max\":" + numToStr(latencyMax()) +
           ",\"mean\":" + dblToStr(latencyMean()) +
           ",\"p50\":" + dblToStr(latencyPercentile(50)) +
           ",\"p95\":" + dblToStr(latencyPercentile(95)) +
           ",\"p99\":" + dblToStr(latencyPercentile(99)) + "}";
    // corrupt counts damage as well as packets: "affected" says how many
    // packets it touched, this says how hard, which is the number an operator
    // compares against the configured bit error rate.
    out += ",\"corrupt\":{\"bitsFlipped\":" + numToStr(corruptGetBitsFlipped()) + "}";
    out += ",\"modules\":{";
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        Module *m = modules[ix];
        if (ix) out += ",";
        out += quoted(m->shortName) + ":{\"enabled\":" +
               std::string(*m->enabledFlag ? "true" : "false") +
               ",\"affected\":" + numToStr(m->affectedCount) + "}";
    }
    out += "}}";
    return out;
}

std::string apiStatusJson() {
    std::string out = "{\"status\":\"ok\"";
    out += ",\"version\":" + quoted(CLUMSY_VERSION);
    out += ",\"capturing\":" + std::string(appIsCapturing() ? "true" : "false");
    out += ",\"admin\":" + std::string(IsRunAsAdmin() ? "true" : "false");
    out += ",\"filter\":" + quoted(appCurrentFilter());
    out += ",\"process\":" + quoted(appCurrentProcess());
    out += ",\"message\":" + quoted(appStatusLine());
    out += "}";
    return out;
}

std::string apiProfilesJson() {
    std::string out = "{\"status\":\"ok\",\"profiles\":[";
    char names[MAX_PROFILES][PROFILE_NAME_SIZE];
    int cnt = profileNamesSnapshot(names, MAX_PROFILES);
    for (int i = 0; i < cnt; ++i) {
        if (i) out += ",";
        out += quoted(names[i]);
    }
    out += "]}";
    return out;
}

std::string apiPresetsJson() {
    std::string out = "{\"status\":\"ok\",\"presets\":[";
    int cnt = appPresetCount();
    for (int i = 0; i < cnt; ++i) {
        if (i) out += ",";
        out += "{\"name\":" + quoted(appPresetName(i)) +
               ",\"filter\":" + quoted(appPresetFilter(i)) + "}";
    }
    out += "]}";
    return out;
}

std::string apiHealthJson() {
    // Deliberately cheap and auth-free: CI health checks poll this.
    return "{\"status\":\"ok\",\"version\":\"" CLUMSY_VERSION "\",\"capturing\":" +
           std::string(appIsCapturing() ? "true" : "false") + "}";
}

// GET /api/docs. A client that has never seen clumsy - a CI script, or a
// language model told only "there is a clumsy on port 8080" - should be able to
// drive it from this reply alone, so every entry carries a description and,
// for POST, a body it can copy. The table is serialized through quoted()
// rather than kept as one pre-escaped literal: triple-escaped JSON was the
// reason half of these had no description.
std::string apiDocsJson() {
    struct Endpoint {
        const char *method;
        const char *path;
        bool        auth;
        const char *body;   // example request body, or NULL
        const char *desc;
    };
    static const Endpoint endpoints[] = {
        { "GET",  "/api/health", false, NULL,
          "liveness probe; poll this after launch until it answers" },
        { "GET",  "/api/status", true, NULL,
          "capturing (bool), admin (bool: can this instance capture at all), "
          "filter, process, last status message" },
        { "GET",  "/api/modules", true, NULL,
          "every module: shortName, enabled, current params, and paramSpecs "
          "(key, type, min, max, options) - the authoritative list of keys" },
        { "GET",  "/api/stats", true, NULL,
          "live counters: captured, sent, per-module affected, latency "
          "percentiles; use it to verify a module is really acting" },
        { "GET",  "/api/stream", true, NULL,
          "Server-Sent Events, one stats frame every 200ms; never returns, "
          "prefer /api/stats from a script" },
        { "GET",  "/api/profiles", true, NULL, "profile names from profiles.json" },
        { "GET",  "/api/presets", true, NULL, "named filter expressions from config.json" },
        { "GET",  "/api/report", true, NULL, "HTML session report download" },
        { "GET",  "/api/docs", true, NULL, "this document" },
        { "GET",  "/llms.txt", true, NULL,
          "plain-text usage guide for AI agents: workflow, filter syntax, safety rules" },
        { "GET",  "/llms_kr.txt", true, NULL, "the same guide in Korean" },
        { "GET",  "/metrics", false, NULL, "Prometheus text exposition" },
        { "POST", "/api/filter", true,
          "{\"filter\":\"udp and udp.DstPort == 7777\",\"process\":\"game.exe\"}",
          "start capturing with this filter (restarts if already capturing); "
          "process is optional; nothing is shaped until this succeeds" },
        { "POST", "/api/stop", true, NULL,
          "stop capturing; traffic flows normally again, clumsy keeps running" },
        { "POST", "/api/quit", true, NULL, "stop capturing and shut clumsy down" },
        { "POST", "/api/modules/{shortName}", true,
          "{\"enabled\":true,\"lag-time\":100}",
          "switch one module on or off and/or set its params; the reply "
          "carries the resulting params" },
        { "POST", "/api/apply", true,
          "{\"lag\":true,\"lag-time\":200,\"drop\":true,\"drop-chance\":5}",
          "set several modules in one request: a module shortName enables or "
          "disables it, any other key is a param" },
        { "POST", "/api/profiles", true, "{\"name\":\"mobile-4g\"}",
          "save the current module state as a profile" },
        { "POST", "/api/profiles/{name}/apply", true, NULL, "apply a saved profile" },
        { "POST", "/api/profiles/{name}/delete", true, NULL,
          "remove a profile from profiles.json" },
        { "POST", "/api/presets", true,
          "{\"name\":\"my filter\",\"filter\":\"udp\"}",
          "save a named filter expression to config.json" },
        { "POST", "/api/scenario/loadinline", true,
          "{\"scenario\":[{\"at\":0,\"lag\":true,\"lag-time\":100},"
          "{\"at\":30,\"lag\":false}]}",
          "load a scenario from the body: an array of steps, each with an "
          "\"at\" (seconds since the capture started) or \"when\" trigger plus "
          "the same keys /api/apply takes; it plays when a capture starts" },
        { "POST", "/api/scenario/load", true, "{\"path\":\"scenario.json\"}",
          "load a scenario file from the machine clumsy runs on" },
        { "POST", "/api/scenario/start", true, NULL,
          "restart playback of the loaded scenario (it also starts with each capture)" },
        { "POST", "/api/scenario/stop", true, NULL, "halt scenario playback" },
        { "POST", "/api/pcap/start", true,
          "{\"path\":\"out.pcap\",\"maxPackets\":0,\"maxBytes\":0}",
          "dump captured packets to a libpcap file (0 = unlimited)" },
        { "POST", "/api/pcap/stop", true, NULL, "close the pcap file" },
        { "POST", "/api/replay/start", true,
          "{\"path\":\"in.pcap\",\"speed\":1.0,\"loop\":false}",
          "re-inject a saved libpcap file" },
        { "POST", "/api/replay/stop", true, NULL, "halt pcap replay" },
    };
    static const char *const conventions[] = {
        "Only GET and POST exist. POST bodies are JSON objects.",
        "Every JSON reply has \"status\": \"ok\" or \"error\"; errors add \"message\".",
        "A reply can be \"ok\" and still carry \"unknownKeys\" (keys no module "
        "has) or \"warnings\" (values clamped or not applied). Read both. A "
        "request that applied nothing at all is an error (400).",
        "Module settings do nothing until a capture is running: POST /api/filter "
        "first. \"capturing\": false in a reply means the setting is stored but "
        "no traffic is being shaped yet.",
        "Chances are percentages 0..100 (5 means 5%), times are milliseconds, "
        "bandwidth is kilobytes per second. Booleans may be true/false, "
        "\"on\"/\"off\" or 1/0.",
        "No token is needed by default. Only when clumsy was started with "
        "--web-token or a non-loopback --web-bind, send it as the "
        "X-Clumsy-Token header or ?token= on every endpoint except those "
        "marked \"auth\": false.",
    };

    std::string out = "{\"status\":\"ok\",\"version\":" + quoted(CLUMSY_VERSION) +
                      ",\"guide\":\"/llms.txt\",\"conventions\":[";
    for (size_t i = 0; i < sizeof(conventions) / sizeof(conventions[0]); ++i) {
        if (i) out += ",";
        out += quoted(conventions[i]);
    }
    out += "],\"endpoints\":[";
    for (size_t i = 0; i < sizeof(endpoints) / sizeof(endpoints[0]); ++i) {
        const Endpoint &e = endpoints[i];
        if (i) out += ",";
        out += "{\"method\":" + quoted(e.method) + ",\"path\":" + quoted(e.path);
        // Only the exceptions are marked, as before: "auth": true on nearly
        // every line reads as "a token is required", which by default it is not.
        if (!e.auth) out += ",\"auth\":false";
        if (e.body) out += ",\"body\":" + quoted(e.body);
        out += ",\"desc\":" + quoted(e.desc) + "}";
    }
    out += "]}";
    return out;
}

// ---------------------------------------------------------------------------
// Prometheus text exposition format v0.0.4  (T1)
//
// Counters and gauges only: no summaries, and the one histogram is emitted in
// the native cumulative-bucket form so histogram_quantile() works in Grafana
// against exactly the same numbers /api/stats reports.
// ---------------------------------------------------------------------------
std::string apiMetricsText() {
    std::string o;

    auto metric = [&](const char *name, const char *help, const char *type,
                      const std::string &value) {
        o += "# HELP clumsy_"; o += name; o += " "; o += help; o += "\n";
        o += "# TYPE clumsy_"; o += name; o += " "; o += type; o += "\n";
        o += "clumsy_"; o += name; o += " "; o += value; o += "\n";
    };
    auto boolStr = [](bool b) { return std::string(b ? "1" : "0"); };

    metric("up", "always 1; the scrape succeeded", "gauge", "1");
    metric("capturing", "1 while a capture is running", "gauge",
           boolStr(appIsCapturing() != 0));
    metric("captured_packets_total", "packets taken off the wire", "counter",
           numToStr(statsCapturedTotal));
    metric("sent_packets_total", "packets handed back to the stack", "counter",
           numToStr(statsSentTotal));
    metric("capture_elapsed_seconds", "seconds since the capture started", "gauge",
           dblToStr((double)appCaptureElapsedMs() / 1000.0));

    metric("lag_buffer_packets", "packets held by the lag module", "gauge",
           numToStr(lagGetBufSize()));
    metric("jitter_buffer_packets", "packets held by the jitter module", "gauge",
           numToStr(jitterGetBufSize()));
    metric("bandwidth_buffer_packets", "packets held by the bandwidth module", "gauge",
           numToStr(bandwidthGetBufSize()));

    metric("pcap_written_packets_total", "packets written to the pcap file", "counter",
           numToStr(pcapExportCount()));
    metric("pcap_written_bytes_total", "bytes written to the pcap file", "counter",
           numToStr(pcapExportBytes()));
    metric("replay_active", "1 while a pcap replay is running", "gauge",
           boolStr(pcapReplayIsActive() != 0));
    metric("replay_injected_packets_total", "packets re-injected from a pcap file",
           "counter", numToStr(pcapReplaySent()));
    metric("replay_failed_packets_total", "replay packets that could not be injected",
           "counter", numToStr(pcapReplayFailed()));
    metric("corrupt_flipped_bits_total", "bits the corrupt module flipped",
           "counter", numToStr(corruptGetBitsFlipped()));

    // Per-module series. shortName is a label rather than part of the metric
    // name, which is what lets one Grafana panel cover every module.
    o += "# HELP clumsy_module_enabled 1 while a module is switched on\n";
    o += "# TYPE clumsy_module_enabled gauge\n";
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        o += "clumsy_module_enabled{module=\"" + std::string(modules[ix]->shortName) +
             "\"} " + (*modules[ix]->enabledFlag ? "1" : "0") + "\n";
    }
    o += "# HELP clumsy_module_affected_packets_total packets a module acted on\n";
    o += "# TYPE clumsy_module_affected_packets_total counter\n";
    for (int ix = 0; ix < MODULE_CNT; ++ix) {
        o += "clumsy_module_affected_packets_total{module=\"" +
             std::string(modules[ix]->shortName) + "\"} " +
             numToStr(modules[ix]->affectedCount) + "\n";
    }

    // Native histogram: cumulative le= buckets, then _sum and _count.
    o += "# HELP clumsy_latency_ms observed packet delay from lag and jitter\n";
    o += "# TYPE clumsy_latency_ms histogram\n";
    {
        long cumulative = 0;
        for (int i = 0; i < LATENCY_BUCKETS; ++i) {
            cumulative += latencyBucketCount(i);
            LONG upper = latencyBucketUpper(i);
            std::string le = (upper != 0) ? numToStr(upper) : std::string("+Inf");
            o += "clumsy_latency_ms_bucket{le=\"" + le + "\"} " +
                 numToStr(cumulative) + "\n";
        }
        o += "clumsy_latency_ms_sum " + dblToStr(latencySumMs()) + "\n";
        o += "clumsy_latency_ms_count " + numToStr(latencyCount()) + "\n";
    }

    return o;
}

// ---------------------------------------------------------------------------
// Mutating handlers
// ---------------------------------------------------------------------------

// Applies every non-meta key in a request object to the module's setParam.
// Returns the number of parameters that were applied. Keys the module does not
// have go to unknownOut; values that were clamped, or refused outright, are
// explained in warningsOut. Either may be null for callers that keep the
// pre-Phase-2 reply shape and have nowhere to put them.
static int applyModuleObject(const JsonValue &req, Module *mod, std::string *unknownOut,
                             std::vector<std::string> *warningsOut) {
    int applied = 0;
    if (!mod->setParam || !req.isObject()) return 0;
    for (size_t i = 0; i < req.obj.size(); ++i) {
        const std::string &key = req.obj[i].first;
        if (key == "cmd" || key == "module" || key == "enabled") continue;
        std::string val = req.obj[i].second.asString();
        char msg[MSG_BUFSIZE];
        int verdict = applyModuleParamChecked(mod, key.c_str(), val.c_str(), msg, sizeof(msg));

        if (verdict == PARAM_OK || verdict == PARAM_ADJUSTED) applied++;
        if (verdict == PARAM_UNKNOWN && unknownOut) {
            if (!unknownOut->empty()) *unknownOut += ",";
            *unknownOut += key;
        }
        if (verdict == PARAM_ADJUSTED && warningsOut) warningsOut->push_back(msg);
        if (verdict == PARAM_INVALID && warningsOut) {
            warningsOut->push_back(std::string(msg) + " - not applied");
        }
    }
    return applied;
}

// Reads the "enabled" member through the shared boolean vocabulary, so true,
// "true", "on" and 1 all mean the same thing on every transport. Returns false
// and leaves the module alone when the value is none of those words.
static bool applyEnabledField(const JsonValue &req, Module *mod,
                              std::vector<std::string> *warningsOut) {
    const JsonValue *en = req.find("enabled");
    if (!en) return false;
    std::string word = en->asString();
    if (!isBoolWord(word.c_str())) {
        if (warningsOut) {
            warningsOut->push_back("enabled: \"" + word +
                                   "\" is not a boolean, use true or false - not applied");
        }
        return false;
    }
    InterlockedExchange16(mod->enabledFlag, (short)parseBoolValue(word.c_str()));
    return true;
}

// The error text for a request that asked for changes and made none. "ok" with
// "applied":0 reads as success to anything that checks the status alone, and a
// request made entirely of guessed names ("latency", "loss") is exactly the
// one whose sender needs to be told it did nothing.
static std::string nothingAppliedMessage(const std::string &unknown,
                                         const std::vector<std::string> &warnings) {
    std::string msg = "nothing was applied.";
    if (!unknown.empty()) msg += " Unknown keys: " + unknown + ".";
    for (size_t i = 0; i < warnings.size(); ++i) msg += " " + warnings[i] + ".";
    return msg;
}

std::string apiSetModule(const std::string &shortName, const std::string &body,
                         int *httpStatus) {
    JsonValue req;
    Module *mod = findModule(shortName);
    std::string unknown;
    std::vector<std::string> warnings;

    *httpStatus = 200;
    if (!mod) {
        *httpStatus = 404;
        return errJson("unknown module: " + shortName + " (modules: " + moduleNameList() + ")");
    }
    if (!body.empty() && !jsonParse(body, req)) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }

    bool enabledApplied = applyEnabledField(req, mod, &warnings);
    if (enabledApplied) {
        INFO("api: %s %s", shortName.c_str(), *mod->enabledFlag ? "enabled" : "disabled");
    }
    int applied = applyModuleObject(req, mod, &unknown, &warnings);

    if (applied == 0 && !enabledApplied && (!unknown.empty() || !warnings.empty())) {
        *httpStatus = 400;
        return errJson(nothingAppliedMessage(unknown, warnings) + " " + shortName +
                       " takes: enabled," + moduleKeyList(mod));
    }

    // "params" is the state after this request, so a caller can see in the
    // same reply what a clamped or refused value left behind. "applied" counts
    // parameters; the switch is reported separately as "enabled".
    std::string extra = "\"module\":" + quoted(shortName) +
                        ",\"applied\":" + numToStr(applied) +
                        ",\"enabled\":" + std::string(*mod->enabledFlag ? "true" : "false") +
                        ",\"capturing\":" + std::string(appIsCapturing() ? "true" : "false") +
                        ",\"params\":" + moduleParamsJson(mod);
    if (!unknown.empty()) {
        extra += ",\"unknownKeys\":" + quoted(unknown) +
                 ",\"validKeys\":" + quoted(moduleKeyList(mod));
    }
    extra += warningsField(warnings);
    return okJson(extra);
}

// A refused capture has to leave a trace beyond the one reply that carried it:
// GET /api/status advertises the last status message and the console is where
// an operator looks, and both kept saying "Idle." after a failed start.
static std::string noteCaptureFailure(const char *err) {
    std::string reason = (err && err[0]) ? err : "failed to start capture";
    showStatus(("Capture not started: " + reason).substr(0, MSG_BUFSIZE - 1).c_str());
    return reason;
}

std::string apiSetFilter(const std::string &body, int *httpStatus) {
    JsonValue req;
    char err[MSG_BUFSIZE] = "";

    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }

    std::string filter  = req.str("filter");
    std::string process = req.str("process");

    if (filter.empty()) {
        *httpStatus = 400;
        return errJson("missing 'filter' field");
    }

    // Restarting with a new filter is the common case from the dashboard.
    if (appIsCapturing()) appStopCapture();

    if (!appStartCapture(filter.c_str(), process.c_str(), err, sizeof(err))) {
        *httpStatus = 400;
        return errJson(noteCaptureFailure(err));
    }
    return okJson("\"filter\":" + quoted(filter) + ",\"process\":" + quoted(process));
}

std::string apiStopCapture(int *httpStatus) {
    *httpStatus = 200;
    if (!appIsCapturing()) return okJson("\"message\":\"not capturing\"");
    appStopCapture();
    return okJson("\"message\":\"stopped\"");
}

std::string apiQuit(int *httpStatus) {
    *httpStatus = 200;
    appRequestQuit();
    return okJson("\"message\":\"quitting\"");
}

std::string apiApplyProfile(const std::string &name, int *httpStatus) {
    *httpStatus = 200;
    if (!profileApply(name.c_str())) {
        *httpStatus = 404;
        return errJson("profile not found: " + name);
    }
    return okJson("\"profile\":" + quoted(name));
}

std::string apiSaveProfile(const std::string &body, int *httpStatus) {
    JsonValue req;
    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    std::string name = req.str("name");
    if (name.empty()) {
        *httpStatus = 400;
        return errJson("missing 'name' field");
    }
    if (!profileSaveCurrent(name.c_str())) {
        *httpStatus = 500;
        return errJson("failed to save profile (limit reached?)");
    }
    return okJson("\"profile\":" + quoted(name));
}

std::string apiSavePreset(const std::string &body, int *httpStatus) {
    JsonValue req;
    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    std::string name   = req.str("name");
    std::string filter = req.str("filter");
    if (name.empty() || filter.empty()) {
        *httpStatus = 400;
        return errJson("both 'name' and 'filter' are required");
    }
    if (!appPresetSave(name.c_str(), filter.c_str())) {
        *httpStatus = 500;
        return errJson("failed to write config.json");
    }
    return okJson("\"preset\":" + quoted(name));
}

std::string apiScenarioLoad(const std::string &body, int *httpStatus) {
    JsonValue req;
    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    std::string path = req.str("path");
    if (path.empty()) {
        *httpStatus = 400;
        return errJson("missing 'path' field");
    }
    scenarioLoad(path.c_str());
    if (!scenarioIsLoaded()) {
        *httpStatus = 400;
        return errJson("failed to load scenario: " + path);
    }
    return okJson("\"steps\":" + numToStr(scenarioStepCount()) + scenarioWarningsField());
}

std::string apiScenarioStart(int *httpStatus) {
    *httpStatus = 200;
    if (!scenarioIsLoaded()) {
        *httpStatus = 400;
        return errJson("no scenario loaded");
    }
    scenarioStart();
    std::string extra = "\"steps\":" + numToStr(scenarioStepCount());
    // Steps are only ticked while capturing, and starting a capture restarts
    // the scenario from zero, so without one this call changes nothing yet.
    if (!appIsCapturing()) {
        extra += warningsField({ "no capture is running: steps are timed from the capture "
                                 "start, so none fires until POST /api/filter succeeds" });
    }
    return okJson(extra);
}

std::string apiScenarioStop(int *httpStatus) {
    *httpStatus = 200;
    scenarioStop();
    return okJson();
}

std::string apiPcapStart(const std::string &body, int *httpStatus) {
    JsonValue req;
    *httpStatus = 200;
    if (!body.empty() && !jsonParse(body, req)) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    std::string path = req.str("path", "clumsy.pcap");
    long maxPackets = (long)(req.find("maxPackets") ? req.find("maxPackets")->asNumber() : 0);
    long maxBytes   = (long)(req.find("maxBytes")   ? req.find("maxBytes")->asNumber()   : 0);

    if (!pcapExportStart(path.c_str(), maxPackets, maxBytes)) {
        *httpStatus = 500;
        return errJson("failed to open pcap file: " + path);
    }
    return okJson("\"path\":" + quoted(path));
}

std::string apiPcapStop(int *httpStatus) {
    *httpStatus = 200;
    long n = pcapExportCount();
    pcapExportStop();
    return okJson("\"packets\":" + numToStr(n));
}

std::string apiDeleteProfile(const std::string &name, int *httpStatus) {
    *httpStatus = 200;
    if (!profileDelete(name.c_str())) {
        *httpStatus = 404;
        return errJson("profile not found: " + name);
    }
    return okJson("\"deleted\":" + quoted(name));
}

// Applies a flat {"lag":true,"lag-time":200,...} object across every module in
// one request. The same shape a profile stores, without saving one.
std::string apiApplyInline(const std::string &body, int *httpStatus) {
    JsonValue req;
    std::string unknown, params;
    std::vector<std::string> warnings;
    int applied = 0;

    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    for (size_t i = 0; i < req.obj.size(); ++i) {
        const std::string &key = req.obj[i].first;
        // The pipe sends its whole request object here, "cmd" included.
        if (key == "cmd") continue;
        std::string val = req.obj[i].second.asString();
        char msg[MSG_BUFSIZE];
        int verdict = applyModuleKVChecked(key.c_str(), val.c_str(), msg, sizeof(msg));

        if (verdict == PARAM_OK || verdict == PARAM_ADJUSTED) {
            // Echo each accepted key with the value now in effect, the same
            // service "params" does for POST /api/modules/{name}.
            char now[PARAM_VAL_SIZE];
            Module *owner = findModule(key);
            std::string current = val;
            if (owner) {
                current = *owner->enabledFlag ? "true" : "false";
            } else if (paramSpecFind(key.c_str(), &owner) &&
                       moduleParamCurrent(owner, key.c_str(), now, sizeof(now))) {
                current = now;
            }
            if (!params.empty()) params += ",";
            params += quoted(key) + ":" + quoted(current);
            applied++;
        }
        if (verdict == PARAM_UNKNOWN) {
            if (!unknown.empty()) unknown += ",";
            unknown += key;
        }
        if (verdict == PARAM_ADJUSTED) warnings.push_back(msg);
        if (verdict == PARAM_INVALID)  warnings.push_back(std::string(msg) + " - not applied");
    }

    if (applied == 0 && (!unknown.empty() || !warnings.empty())) {
        *httpStatus = 400;
        return errJson(nothingAppliedMessage(unknown, warnings) +
                       " A key is a module name (" + moduleNameList() +
                       ") to switch it on or off, or one of its parameters; "
                       "GET /api/modules lists them.");
    }

    std::string extra = "\"applied\":" + numToStr(applied) +
                        ",\"capturing\":" + std::string(appIsCapturing() ? "true" : "false") +
                        ",\"params\":{" + params + "}";
    if (!unknown.empty()) extra += ",\"unknownKeys\":" + quoted(unknown);
    extra += warningsField(warnings);
    return okJson(extra);
}

std::string apiScenarioLoadInline(const std::string &body, int *httpStatus) {
    JsonValue req;
    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    // The steps may arrive as a real JSON array or as that array inside a
    // string. The string form came first; the array is what a client writes
    // when nobody has told it otherwise, and refusing it only bought a retry
    // with a layer of backslashes. The step parser works on text either way,
    // so an array is handed over exactly as it was sent.
    std::string doc;
    const JsonValue *sc = req.find("scenario");
    if (sc && sc->isArray()) {
        size_t key  = body.find("\"scenario\"");
        size_t open = (key == std::string::npos) ? key : body.find('[', key);
        if (open != std::string::npos) doc = body.substr(open);
    } else if (sc) {
        doc = sc->asString();
    }
    if (doc.empty()) {
        *httpStatus = 400;
        return errJson("missing 'scenario' field: a JSON array of steps, "
                       "e.g. {\"scenario\":[{\"at\":0,\"lag\":true,\"lag-time\":100}]}");
    }
    scenarioLoadString(doc.c_str());
    if (!scenarioIsLoaded()) {
        *httpStatus = 400;
        return errJson("no usable steps: every entry needs an 'at' or a 'when' trigger");
    }
    return okJson("\"steps\":" + numToStr(scenarioStepCount()) + scenarioWarningsField());
}

std::string apiReplayStart(const std::string &body, int *httpStatus) {
    JsonValue req;
    char err[MSG_BUFSIZE] = "";

    *httpStatus = 200;
    if (!body.empty() && !jsonParse(body, req)) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    std::string path = req.str("path");
    if (path.empty()) {
        *httpStatus = 400;
        return errJson("missing 'path' field");
    }
    double speed = req.find("speed") ? req.find("speed")->asNumber() : 1.0;
    bool loop    = req.find("loop")  ? req.find("loop")->asBool()    : false;

    if (!pcapReplayStart(path.c_str(), speed, loop ? 1 : 0, err, sizeof(err))) {
        *httpStatus = 400;
        return errJson(err[0] ? err : "failed to start replay");
    }
    return okJson("\"path\":" + quoted(path) + ",\"speed\":" + dblToStr(speed));
}

std::string apiReplayStop(int *httpStatus) {
    *httpStatus = 200;
    // Count after the join, not before: the replay thread keeps injecting
    // until it notices the stop flag, and at a high --replay-speed that is
    // thousands of packets the caller would otherwise never hear about.
    pcapReplayStop();
    return okJson("\"injected\":" + numToStr(pcapReplaySent()));
}

// ---------------------------------------------------------------------------
// Legacy Named Pipe protocol, expressed on top of the handlers above
// ---------------------------------------------------------------------------

std::string controlDispatchJson(const std::string &request) {
    JsonValue req;
    int status = 200;

    if (!jsonParse(request, req) || !req.isObject()) {
        return errJson("malformed JSON request");
    }

    const JsonValue *cmdVal = req.find("cmd");
    if (!cmdVal) return errJson("missing cmd field");
    std::string cmd = cmdVal->asString();

    if (cmd == "set") {
        std::string modName = req.str("module");
        if (modName.empty()) return errJson("missing module field");
        Module *mod = findModule(modName);
        if (!mod) return errJson("unknown module: " + modName + " (modules: " + moduleNameList() + ")");

        std::string unknown;
        std::vector<std::string> warnings;
        if (applyEnabledField(req, mod, &warnings)) {
            LOG("pipe: %s enabled=%d", modName.c_str(), (int)*mod->enabledFlag);
        }
        applyModuleObject(req, mod, &unknown, &warnings);
        // Response shape kept byte-compatible with the pre-Phase-2 pipe API so
        // existing automation scripts keep working (Phase 2.9): a request with
        // nothing wrong in it gets exactly the old reply. The extra members
        // only appear when a key or a value was not taken, which used to be
        // reported as plain success.
        std::string out = "{\"status\":\"ok\",\"module\":" + quoted(modName);
        if (!unknown.empty()) out += ",\"unknownKeys\":" + quoted(unknown);
        out += warningsField(warnings) + "}";
        return out;
    }

    if (cmd == "get_stats") {
        // Legacy shape: flat modules map plus captured/sent totals.
        std::string out = "{\"status\":\"ok\",\"modules\":{";
        for (int ix = 0; ix < MODULE_CNT; ++ix) {
            Module *m = modules[ix];
            if (ix) out += ",";
            out += quoted(m->shortName) + ":{\"enabled\":" +
                   std::string(*m->enabledFlag ? "true" : "false") +
                   ",\"affected\":" + numToStr(m->affectedCount) + "}";
        }
        out += "},\"captured\":" + numToStr(statsCapturedTotal) +
               ",\"sent\":" + numToStr(statsSentTotal) + "}";
        return out;
    }

    if (cmd == "stop") {
        // Legacy meaning of "stop" is "shut the program down", not "stop capture".
        InterlockedExchange16(&pipeStopRequested, 1);
        LOG("pipe: stop requested");
        return "{\"status\":\"ok\",\"message\":\"stopping\"}";
    }

    // --- commands added in Phase 2.5, available on both transports ---
    if (cmd == "get_modules")  return apiModulesJson();
    if (cmd == "get_status")   return apiStatusJson();
    if (cmd == "get_presets")  return apiPresetsJson();
    if (cmd == "get_profiles") return apiProfilesJson();
    if (cmd == "filter" || cmd == "start") {
        std::string filter  = req.str("filter");
        std::string process = req.str("process");
        char err[MSG_BUFSIZE] = "";
        if (filter.empty()) return errJson("missing filter field");
        if (appIsCapturing()) appStopCapture();
        if (!appStartCapture(filter.c_str(), process.c_str(), err, sizeof(err))) {
            return errJson(noteCaptureFailure(err));
        }
        return okJson("\"filter\":" + quoted(filter));
    }
    if (cmd == "stop_capture") return apiStopCapture(&status);
    if (cmd == "quit")         return apiQuit(&status);
    if (cmd == "profile") {
        std::string name = req.str("name");
        if (name.empty()) return errJson("missing name field");
        return apiApplyProfile(name, &status);
    }
    if (cmd == "scenario") {
        std::string action = req.str("action", "start");
        if (action == "load")  return apiScenarioLoad(request, &status);
        if (action == "stop")  return apiScenarioStop(&status);
        return apiScenarioStart(&status);
    }
    if (cmd == "pcap") {
        std::string action = req.str("action", "start");
        if (action == "stop") return apiPcapStop(&status);
        return apiPcapStart(request, &status);
    }
    if (cmd == "replay") {
        std::string action = req.str("action", "start");
        if (action == "stop") return apiReplayStop(&status);
        return apiReplayStart(request, &status);
    }
    if (cmd == "apply")   return apiApplyInline(request, &status);
    if (cmd == "metrics") {
        // Every other reply on this transport is a JSON object with a "status"
        // field, and pipe.cpp documents that as the contract, so the raw
        // Prometheus text goes inside the envelope rather than replacing it.
        // Scrapers should use GET /metrics, which still serves it verbatim.
        return okJson("\"metrics\":" + quoted(apiMetricsText()));
    }
    if (cmd == "delete_profile") {
        std::string name = req.str("name");
        if (name.empty()) return errJson("missing name field");
        return apiDeleteProfile(name, &status);
    }

    return errJson("unknown cmd: " + cmd);
}
