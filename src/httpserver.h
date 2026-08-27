// Embedded HTTP server  (Phase 2.4).
#pragma once

// Starts the listener thread. bindAddr defaults to 127.0.0.1 when NULL.
// token may be NULL, in which case one is generated whenever authentication is
// required (i.e. whenever bindAddr is not a loopback address).
// Returns 1 on success, 0 on failure.
int  httpServerStart(const char *bindAddr, int port, const char *token);
void httpServerStop(void);

// "http://127.0.0.1:8080/" or "http://0.0.0.0:8080/?token=..." — for the banner.
const char* httpServerUrl(void);
// Empty string when authentication is not required.
const char* httpServerToken(void);
