// Transport-independent control layer  (Phase 2.5).
//
// Every command clumsy accepts lives here exactly once. pipe.cpp wraps it in
// the legacy Named Pipe protocol and httpserver.cpp wraps it in REST, so the
// two transports can never drift apart.
#pragma once

#include <string>

// ---------------------------------------------------------------------------
// Legacy Named Pipe protocol: one JSON request object in, one JSON object out.
// Recognised commands: set, get_stats, get_modules, get_status, filter, start,
// stop, quit, profile, scenario.
// ---------------------------------------------------------------------------
std::string controlDispatchJson(const std::string &request);

// ---------------------------------------------------------------------------
// REST payload builders
// ---------------------------------------------------------------------------
std::string apiModulesJson();    // GET  /api/modules
std::string apiStatsJson();      // GET  /api/stats   (also the SSE payload)
std::string apiStatusJson();     // GET  /api/status
std::string apiProfilesJson();   // GET  /api/profiles
std::string apiPresetsJson();    // GET  /api/presets
std::string apiHealthJson();     // GET  /api/health  (no auth)
std::string apiDocsJson();       // GET  /api/docs
std::string apiMetricsText();    // GET  /metrics     (Prometheus, no auth)

// ---------------------------------------------------------------------------
// REST handlers. body is the raw request body; *httpStatus receives the status
// code to send (200 / 400 / 404 / 500).
// ---------------------------------------------------------------------------
std::string apiSetModule(const std::string &shortName, const std::string &body, int *httpStatus);
std::string apiSetFilter(const std::string &body, int *httpStatus);
std::string apiStopCapture(int *httpStatus);
std::string apiQuit(int *httpStatus);
std::string apiApplyProfile(const std::string &name, int *httpStatus);
std::string apiSaveProfile(const std::string &body, int *httpStatus);
std::string apiSavePreset(const std::string &body, int *httpStatus);
std::string apiScenarioLoad(const std::string &body, int *httpStatus);
std::string apiScenarioStart(int *httpStatus);
std::string apiScenarioStop(int *httpStatus);
std::string apiPcapStart(const std::string &body, int *httpStatus);
std::string apiPcapStop(int *httpStatus);
std::string apiDeleteProfile(const std::string &name, int *httpStatus);
std::string apiApplyInline(const std::string &body, int *httpStatus);
std::string apiScenarioLoadInline(const std::string &body, int *httpStatus);
std::string apiReplayStart(const std::string &body, int *httpStatus);
std::string apiReplayStop(int *httpStatus);
