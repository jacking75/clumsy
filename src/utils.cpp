#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

// Debug builds trace by default; Release stays quiet until --verbose on.
#ifdef _DEBUG
volatile short logVerbose = 1;
#else
volatile short logVerbose = 0;
#endif

void logPrintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fflush(stdout);
}

int appendf(char *buf, int bufSize, int pos, const char *fmt, ...) {
    va_list args;
    int n;

    if (!buf || pos < 0 || pos >= bufSize - 1) return pos;
    va_start(args, fmt);
    n = vsnprintf(buf + pos, (size_t)(bufSize - pos), fmt, args);
    va_end(args);
    if (n < 0) return pos;
    // snprintf reports what it *would* have written, not what it did. Adding
    // that straight to pos is how a truncating append walks past the end of
    // the buffer and makes the next call's remaining size negative - which,
    // cast to size_t, becomes a licence to write anywhere.
    if (n > bufSize - pos - 1) n = bufSize - pos - 1;
    return pos + n;
}

short calcChance(short chance) {
    // notice that here we made a copy of chance, so even though it's volatile it is still ok
    return (chance == 10000) || ((rand() % 10000) < chance);
}

double randUnit(void) {
    // Uniform in the open interval (0,1). The half-step at both ends is what
    // makes it open rather than half-open, which matters to the callers that
    // feed the result to log(): jitter.cpp's lognormal/pareto draws and
    // corrupt.cpp's geometric bit-error gap both diverge at exactly 0.
    return ((double)rand() + 0.5) / ((double)RAND_MAX + 1.0);
}

static short resolutionSet = 0;

void startTimePeriod() {
    if (!resolutionSet) {
        // begin only fails when period out of range
        timeBeginPeriod(TIMER_RESOLUTION);
        resolutionSet = 1;
    }
}

void endTimePeriod() {
    if (resolutionSet) {
        timeEndPeriod(TIMER_RESOLUTION);
        resolutionSet = 0;
    }
}

// ---------------------------------------------------------------------------
// Shared value helpers used by module setParam implementations
// ---------------------------------------------------------------------------

int parseBoolValue(const char *value) {
    if (!value) return 0;
    return (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
            _stricmp(value, "on") == 0 || _stricmp(value, "yes") == 0) ? 1 : 0;
}

short clampShort(int v, int lo, int hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return I2S(v);
}

LONG clampLong(LONG v, LONG lo, LONG hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

// ---------------------------------------------------------------------------
// Apply a single key-value pair to the appropriate module.
// key is either a module shortName (true/false → enable/disable)
// or a setParam key (e.g. "lag-time", "drop-chance").
// Returns 1 if the key was recognized and applied, 0 otherwise.
// ---------------------------------------------------------------------------
int applyModuleKV(const char *key, const char *value) {
    int j;
    // 1. module shortName → enable / disable
    for (j = 0; j < MODULE_CNT; j++) {
        if (strcmp(modules[j]->shortName, key) == 0) {
            short en = (short)parseBoolValue(value);
            InterlockedExchange16(modules[j]->enabledFlag, en);
            return 1;
        }
    }
    // 2. param key → delegate to the owning module
    for (j = 0; j < MODULE_CNT; j++) {
        if (modules[j]->setParam && modules[j]->setParam(key, value)) {
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// CLI argument store  (Phase 2.1: replaces IupStoreGlobal / IupGetGlobal)
//
// A flat, fixed-size table. Arguments are written once during startup by the
// main thread before any worker thread exists, and only read afterwards, so no
// locking is needed.
// ---------------------------------------------------------------------------
#define MAX_ARGS      64
#define ARG_KEY_SIZE  48
#define ARG_VAL_SIZE  1024

typedef struct {
    char key[ARG_KEY_SIZE];
    char val[ARG_VAL_SIZE];
} ArgEntry;

static ArgEntry argTable[MAX_ARGS];
static int      argCount = 0;

BOOL parameterized = FALSE;

void argSet(const char *key, const char *value) {
    int i;
    if (!key || !value) return;
    for (i = 0; i < argCount; i++) {
        if (strcmp(argTable[i].key, key) == 0) {
            strncpy(argTable[i].val, value, ARG_VAL_SIZE - 1);
            argTable[i].val[ARG_VAL_SIZE - 1] = '\0';
            return;
        }
    }
    if (argCount >= MAX_ARGS) {
        INFO("warning: too many arguments, ignoring --%s", key);
        return;
    }
    strncpy(argTable[argCount].key, key, ARG_KEY_SIZE - 1);
    argTable[argCount].key[ARG_KEY_SIZE - 1] = '\0';
    strncpy(argTable[argCount].val, value, ARG_VAL_SIZE - 1);
    argTable[argCount].val[ARG_VAL_SIZE - 1] = '\0';
    argCount++;
}

const char* argGet(const char *key) {
    int i;
    if (!key) return NULL;
    for (i = 0; i < argCount; i++) {
        if (strcmp(argTable[i].key, key) == 0) return argTable[i].val;
    }
    return NULL;
}

int argGetInt(const char *key, int defVal) {
    const char *v = argGet(key);
    return v ? atoi(v) : defVal;
}

// parse arguments and set globals
// only checks for argument style, no extra validation is done
BOOL parseArgs(int argc, char* argv[]) {
    int ix = 0;
    char *key, *value;
    // begin parsing "--key value" args.
    // notice that quoted arg with spaces in between is already handled by shell
    if (argc == 1) return FALSE;
    for (;;) {
        if (++ix >= argc) break;
        key = argv[ix];
        if (key[0] != '-' || key[1] != '-' || key[2] == '\0') {
            return FALSE;
        }
        key = &(key[2]); // skip "--"
        if (++ix >= argc) {
            return FALSE;
        }
        value = argv[ix];
        argSet(key, value);
    }

    return TRUE;
}
