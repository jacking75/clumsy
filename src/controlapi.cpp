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
    if (v == (double)(long long)v) snprintf(buf, sizeof(buf), "%lld", (long long)v);
    else                           snprintf(buf, sizeof(buf), "%.4g", v);
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
    int cnt = profileCount();
    for (int i = 0; i < cnt; ++i) {
        if (i) out += ",";
        out += quoted(profileGetName(i));
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

std::string apiDocsJson() {
    static const char *docs =
    "{\"status\":\"ok\",\"endpoints\":["
    "{\"method\":\"GET\",\"path\":\"/api/health\",\"auth\":false,\"desc\":\"liveness probe\"},"
    "{\"method\":\"GET\",\"path\":\"/api/status\",\"desc\":\"capture state, filter, last message\"},"
    "{\"method\":\"GET\",\"path\":\"/api/modules\",\"desc\":\"all modules with values and ParamSpec form metadata\"},"
    "{\"method\":\"GET\",\"path\":\"/api/stats\",\"desc\":\"live counters\"},"
    "{\"method\":\"GET\",\"path\":\"/api/stream\",\"desc\":\"Server-Sent Events, one stats frame every 200ms\"},"
    "{\"method\":\"GET\",\"path\":\"/api/profiles\",\"desc\":\"profile names from profiles.json\"},"
    "{\"method\":\"GET\",\"path\":\"/api/presets\",\"desc\":\"filter presets from config.json\"},"
    "{\"method\":\"GET\",\"path\":\"/api/report\",\"desc\":\"HTML session report download\"},"
    "{\"method\":\"GET\",\"path\":\"/api/docs\",\"desc\":\"this document\"},"
    "{\"method\":\"POST\",\"path\":\"/api/modules/{shortName}\",\"body\":\"{\\\"enabled\\\":true,\\\"lag-time\\\":100}\"},"
    "{\"method\":\"POST\",\"path\":\"/api/filter\",\"body\":\"{\\\"filter\\\":\\\"udp and outbound\\\",\\\"process\\\":\\\"game.exe\\\"}\"},"
    "{\"method\":\"POST\",\"path\":\"/api/stop\",\"desc\":\"stop capture\"},"
    "{\"method\":\"POST\",\"path\":\"/api/quit\",\"desc\":\"shut clumsy down\"},"
    "{\"method\":\"POST\",\"path\":\"/api/profiles\",\"body\":\"{\\\"name\\\":\\\"mobile-4g\\\"}\",\"desc\":\"save current state as a profile\"},"
    "{\"method\":\"POST\",\"path\":\"/api/profiles/{name}/apply\",\"desc\":\"apply a profile\"},"
    "{\"method\":\"POST\",\"path\":\"/api/presets\",\"body\":\"{\\\"name\\\":\\\"my filter\\\",\\\"filter\\\":\\\"udp\\\"}\"},"
    "{\"method\":\"POST\",\"path\":\"/api/scenario/load\",\"body\":\"{\\\"path\\\":\\\"scenario.json\\\"}\"},"
    "{\"method\":\"POST\",\"path\":\"/api/scenario/start\",\"desc\":\"restart scenario playback\"},"
    "{\"method\":\"POST\",\"path\":\"/api/scenario/stop\",\"desc\":\"halt scenario playback\"},"
    "{\"method\":\"POST\",\"path\":\"/api/pcap/start\",\"body\":\"{\\\"path\\\":\\\"out.pcap\\\",\\\"maxPackets\\\":0,\\\"maxBytes\\\":0}\"},"
    "{\"method\":\"POST\",\"path\":\"/api/pcap/stop\",\"desc\":\"close the pcap file\"},"
    "{\"method\":\"GET\",\"path\":\"/metrics\",\"auth\":false,\"desc\":\"Prometheus text exposition\"},"
    "{\"method\":\"POST\",\"path\":\"/api/apply\",\"body\":\"{\\\"lag\\\":true,\\\"lag-time\\\":200}\",\"desc\":\"set several modules at once without saving a profile\"},"
    "{\"method\":\"POST\",\"path\":\"/api/profiles/{name}/delete\",\"desc\":\"remove a profile from profiles.json\"},"
    "{\"method\":\"POST\",\"path\":\"/api/scenario/loadinline\",\"body\":\"{\\\"scenario\\\":\\\"[{...}]\\\"}\",\"desc\":\"load a scenario from the request body\"},"
    "{\"method\":\"POST\",\"path\":\"/api/replay/start\",\"body\":\"{\\\"path\\\":\\\"in.pcap\\\",\\\"speed\\\":1.0,\\\"loop\\\":false}\"},"
    "{\"method\":\"POST\",\"path\":\"/api/replay/stop\",\"desc\":\"halt pcap replay\"}"
    "]}";
    return docs;
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
// Returns the number of parameters that were recognised.
static int applyModuleObject(const JsonValue &req, Module *mod, std::string *unknownOut) {
    int applied = 0;
    if (!mod->setParam || !req.isObject()) return 0;
    for (size_t i = 0; i < req.obj.size(); ++i) {
        const std::string &key = req.obj[i].first;
        if (key == "cmd" || key == "module" || key == "enabled") continue;
        std::string val = req.obj[i].second.asString();
        if (mod->setParam(key.c_str(), val.c_str())) {
            applied++;
        } else if (unknownOut) {
            if (!unknownOut->empty()) *unknownOut += ",";
            *unknownOut += key;
        }
    }
    return applied;
}

std::string apiSetModule(const std::string &shortName, const std::string &body,
                         int *httpStatus) {
    JsonValue req;
    Module *mod = findModule(shortName);
    std::string unknown;

    *httpStatus = 200;
    if (!mod) {
        *httpStatus = 404;
        return errJson("unknown module: " + shortName);
    }
    if (!body.empty() && !jsonParse(body, req)) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }

    if (const JsonValue *en = req.find("enabled")) {
        InterlockedExchange16(mod->enabledFlag, (short)(en->asBool() ? 1 : 0));
        INFO("api: %s %s", shortName.c_str(), *mod->enabledFlag ? "enabled" : "disabled");
    }
    int applied = applyModuleObject(req, mod, &unknown);

    std::string extra = "\"module\":" + quoted(shortName) +
                        ",\"applied\":" + numToStr(applied) +
                        ",\"enabled\":" + std::string(*mod->enabledFlag ? "true" : "false");
    if (!unknown.empty()) extra += ",\"unknownKeys\":" + quoted(unknown);
    return okJson(extra);
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
        return errJson(err[0] ? err : "failed to start capture");
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
    return okJson("\"steps\":" + numToStr(scenarioStepCount()));
}

std::string apiScenarioStart(int *httpStatus) {
    *httpStatus = 200;
    if (!scenarioIsLoaded()) {
        *httpStatus = 400;
        return errJson("no scenario loaded");
    }
    scenarioStart();
    return okJson("\"steps\":" + numToStr(scenarioStepCount()));
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
    std::string unknown;
    int applied = 0;

    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    for (size_t i = 0; i < req.obj.size(); ++i) {
        const std::string &key = req.obj[i].first;
        std::string val = req.obj[i].second.asString();
        if (applyModuleKV(key.c_str(), val.c_str())) {
            applied++;
        } else {
            if (!unknown.empty()) unknown += ",";
            unknown += key;
        }
    }
    std::string extra = "\"applied\":" + numToStr(applied);
    if (!unknown.empty()) extra += ",\"unknownKeys\":" + quoted(unknown);
    return okJson(extra);
}

std::string apiScenarioLoadInline(const std::string &body, int *httpStatus) {
    JsonValue req;
    *httpStatus = 200;
    if (!jsonParse(body, req) || !req.isObject()) {
        *httpStatus = 400;
        return errJson("malformed JSON body");
    }
    std::string doc = req.str("scenario");
    if (doc.empty()) {
        *httpStatus = 400;
        return errJson("missing 'scenario' field (a JSON array as a string)");
    }
    scenarioLoadString(doc.c_str());
    if (!scenarioIsLoaded()) {
        *httpStatus = 400;
        return errJson("no usable steps: every entry needs an 'at' or a 'when' trigger");
    }
    return okJson("\"steps\":" + numToStr(scenarioStepCount()));
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
    long sent = pcapReplaySent();
    *httpStatus = 200;
    pcapReplayStop();
    return okJson("\"injected\":" + numToStr(sent));
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
        if (!mod) return errJson("unknown module: " + modName);

        if (const JsonValue *en = req.find("enabled")) {
            InterlockedExchange16(mod->enabledFlag, (short)(en->asBool() ? 1 : 0));
            LOG("pipe: %s enabled=%d", modName.c_str(), (int)*mod->enabledFlag);
        }
        applyModuleObject(req, mod, nullptr);
        // Response shape kept byte-compatible with the pre-Phase-2 pipe API so
        // existing automation scripts keep working (Phase 2.9).
        return "{\"status\":\"ok\",\"module\":" + quoted(modName) + "}";
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
            return errJson(err[0] ? err : "failed to start capture");
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
    if (cmd == "metrics") return apiMetricsText();
    if (cmd == "delete_profile") {
        std::string name = req.str("name");
        if (name.empty()) return errJson("missing name field");
        return apiDeleteProfile(name, &status);
    }

    return errJson("unknown cmd: " + cmd);
}
