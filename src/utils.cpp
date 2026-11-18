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

// The one boolean vocabulary every entry point shares: CLI arguments, REST and
// pipe bodies, scenario steps and profiles. Matching is case-insensitive
// because "True" and "ON" are what a script or a language model writes when it
// has not read this file, and treating those as "off" enables nothing while
// reporting success.
static int isTrueWord(const char *v) {
    return _stricmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
           _stricmp(v, "on") == 0   || _stricmp(v, "yes") == 0;
}

static int isFalseWord(const char *v) {
    return _stricmp(v, "false") == 0 || strcmp(v, "0") == 0 ||
           _stricmp(v, "off") == 0   || _stricmp(v, "no") == 0;
}

int parseBoolValue(const char *value) {
    return (value && isTrueWord(value)) ? 1 : 0;
}

int isBoolWord(const char *value) {
    return (value && (isTrueWord(value) || isFalseWord(value))) ? 1 : 0;
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
// Parameter lookup and value checking
//
// setParam clamps out-of-range numbers and coerces anything it cannot parse
// (atoi("abc") is 0, an unknown enum name becomes the first choice) without a
// word. That is fine for the dashboard, whose form is generated from the same
// ParamSpec table, but a script or a language model typing values by hand gets
// "ok" back for a setting that is not the one it asked for. Everything that
// takes values from outside therefore checks them against the ParamSpec first
// and says what it did.
// ---------------------------------------------------------------------------

// One line on what each module does to traffic. displayName is a label for a
// form ("Throttle", "Out of order"); it does not tell someone choosing between
// drop, burstloss and blackout which one models the condition they were asked
// to reproduce. --help and GET /api/modules both print this.
// Keyed by shortName rather than stored in Module so the plugin ABI and the
// twelve static initializers stay as they are; an unlisted module gets "".
const char* moduleDescription(const char *shortName) {
    static const struct { const char *name; const char *text; } table[] = {
        // Kept under 56 characters so a --help line still fits 80 columns.
        { "lag",       "fixed delay on every packet (constant latency)" },
        { "jitter",    "random delay per packet, between min and max" },
        { "drop",      "random packet loss, evenly spread" },
        { "burstloss", "packet loss in bursts (Gilbert-Elliott good/bad state)" },
        { "blackout",  "total outage for <duration> after each <gap> of traffic" },
        { "throttle",  "hold packets for a time frame, then release in a burst" },
        { "duplicate", "send extra copies of packets" },
        { "ood",       "out of order: deliver packets in a different order" },
        { "tamper",    "overwrite payload bytes (corrupted application data)" },
        { "corrupt",   "flip payload bits at a bit error rate (noisy wireless)" },
        { "reset",     "set TCP RST so connections are torn down" },
        { "bandwidth", "cap throughput at a KB/s limit (slow link)" },
    };
    for (size_t i = 0; shortName && i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcmp(table[i].name, shortName) == 0) return table[i].text;
    }
    return "";
}

const ParamSpec* moduleParamSpec(const Module *m, const char *key) {
    int i;
    if (!m || !key) return NULL;
    for (i = 0; i < m->paramSpecCount; ++i) {
        if (strcmp(m->paramSpecs[i].key, key) == 0) return &m->paramSpecs[i];
    }
    return NULL;
}

const ParamSpec* paramSpecFind(const char *key, Module **ownerOut) {
    int j;
    for (j = 0; j < MODULE_CNT; j++) {
        const ParamSpec *spec = moduleParamSpec(modules[j], key);
        if (spec) {
            if (ownerOut) *ownerOut = modules[j];
            return spec;
        }
    }
    return NULL;
}

int moduleParamCurrent(const Module *m, const char *key, char *buf, int bufSize) {
    ParamKV kv[32];
    int i, n;
    if (!m || !m->getParams || !buf || bufSize <= 0) return 0;
    n = m->getParams(kv, 32);
    for (i = 0; i < n; ++i) {
        if (strcmp(kv[i].key, key) == 0) {
            snprintf(buf, (size_t)bufSize, "%s", kv[i].val);
            return 1;
        }
    }
    return 0;
}

// [+-]digits[.digits] and nothing else. Stricter than strtod on purpose: the
// modules read integers with atoi/atol, which stop at the first character they
// do not like, so "1e3" would pass a strtod check as 1000 and be applied as 1.
static int isPlainDecimal(const char *s, int *hasFraction) {
    int digits = 0;
    *hasFraction = 0;
    if (*s == '+' || *s == '-') s++;
    while (*s >= '0' && *s <= '9') { s++; digits++; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (*s != '0') *hasFraction = 1;
            s++;
        }
    }
    return digits > 0 && *s == '\0';
}

static int enumHasChoice(const char *options, const char *value) {
    const char *p = options;
    size_t vlen = strlen(value);
    int count = 0, hasFraction = 0;

    while (p && *p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == vlen && _strnicmp(p, value, len) == 0) return 1;
        count++;
        if (!comma) break;
        p = comma + 1;
    }
    // The numeric index is accepted too, for scenarios and profiles written
    // before the names existed (see jitterSetParam).
    if (isPlainDecimal(value, &hasFraction) && !hasFraction) {
        int ix = atoi(value);
        return ix >= 0 && ix < count;
    }
    return 0;
}

int paramCheckValue(const ParamSpec *spec, const char *value, char *msg, int msgSize) {
    char scratch[8];
    const char *t = spec->type;
    int hasFraction = 0, hasRange;
    double v;

    // Callers that only want the verdict may pass no buffer.
    if (!msg || msgSize <= 0) { msg = scratch; msgSize = (int)sizeof(scratch); }
    msg[0] = '\0';
    if (!value) value = "";

    if (strcmp(t, "bool") == 0 || strcmp(t, "action") == 0) {
        if (isBoolWord(value)) return PARAM_OK;
        snprintf(msg, (size_t)msgSize, "%s: \"%.40s\" is not a boolean, use on or off",
                 spec->key, value);
        return PARAM_INVALID;
    }

    if (strcmp(t, "enum") == 0) {
        if (enumHasChoice(spec->options, value)) return PARAM_OK;
        snprintf(msg, (size_t)msgSize, "%s: \"%.40s\" is not one of %s",
                 spec->key, value, spec->options ? spec->options : "");
        return PARAM_INVALID;
    }

    // int / float / percent
    hasRange = spec->minVal != spec->maxVal;
    if (!isPlainDecimal(value, &hasFraction)) {
        if (hasRange) {
            snprintf(msg, (size_t)msgSize,
                     "%s: \"%.40s\" is not a number, expected %s %.10g..%.10g",
                     spec->key, value, t, spec->minVal, spec->maxVal);
        } else {
            snprintf(msg, (size_t)msgSize, "%s: \"%.40s\" is not a number",
                     spec->key, value);
        }
        return PARAM_INVALID;
    }
    v = atof(value);
    if (hasRange && (v < spec->minVal || v > spec->maxVal)) {
        snprintf(msg, (size_t)msgSize, "%s: %.40s is outside %.10g..%.10g, clamped to %.10g",
                 spec->key, value, spec->minVal, spec->maxVal,
                 v < spec->minVal ? spec->minVal : spec->maxVal);
        return PARAM_ADJUSTED;
    }
    if (strcmp(t, "int") == 0 && hasFraction) {
        snprintf(msg, (size_t)msgSize,
                 "%s: %.40s is not a whole number, the fraction is dropped",
                 spec->key, value);
        return PARAM_ADJUSTED;
    }
    return PARAM_OK;
}

int applyModuleParamChecked(Module *m, const char *key, const char *value,
                            char *msg, int msgSize) {
    const ParamSpec *spec = moduleParamSpec(m, key);
    int verdict = PARAM_OK;

    if (msg && msgSize > 0) msg[0] = '\0';
    if (!m->setParam) return PARAM_UNKNOWN;
    if (spec) {
        verdict = paramCheckValue(spec, value, msg, msgSize);
        if (verdict == PARAM_INVALID) return PARAM_INVALID;   // left untouched
    }
    // No spec is not a refusal: a plugin module may take keys it never
    // published, and those go through unchecked exactly as before.
    return m->setParam(key, value) ? verdict : PARAM_UNKNOWN;
}

int applyModuleKVChecked(const char *key, const char *value, char *msg, int msgSize) {
    Module *owner = NULL;
    int j;

    if (msg && msgSize > 0) msg[0] = '\0';
    if (!value) value = "";

    // 1. module shortName -> enable / disable
    for (j = 0; j < MODULE_CNT; j++) {
        if (strcmp(modules[j]->shortName, key) == 0) {
            if (!isBoolWord(value)) {
                if (msg && msgSize > 0) {
                    snprintf(msg, (size_t)msgSize,
                             "%s: \"%.40s\" is not a boolean, use true or false",
                             key, value);
                }
                return PARAM_INVALID;
            }
            InterlockedExchange16(modules[j]->enabledFlag, (short)parseBoolValue(value));
            return PARAM_OK;
        }
    }
    // 2. a published parameter -> its owning module, value checked first
    if (paramSpecFind(key, &owner)) {
        return applyModuleParamChecked(owner, key, value, msg, msgSize);
    }
    // 3. unpublished key -> whichever module claims it
    for (j = 0; j < MODULE_CNT; j++) {
        if (modules[j]->setParam && modules[j]->setParam(key, value)) return PARAM_OK;
    }
    return PARAM_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Apply a single key-value pair to the appropriate module, unchecked.
// key is either a module shortName (true/false → enable/disable)
// or a setParam key (e.g. "lag-time", "drop-chance").
// Returns 1 if the key was recognized and applied, 0 otherwise.
//
// This is the path for values clumsy wrote itself (profiles) or already linted
// at load time (scenario steps). Anything typed by a person or sent by a
// client goes through applyModuleKVChecked() above.
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

int argStoredCount(void) { return argCount; }

const char* argStoredKey(int ix) {
    return (ix >= 0 && ix < argCount) ? argTable[ix].key : "";
}

// Parses "--key value" pairs into the argument store. Only the shape is checked
// here; main.cpp decides which keys and values make sense.
// On failure errBuf names the token that broke the pattern, because "invalid
// arguments" alone leaves the caller guessing which of twenty it was.
BOOL parseArgs(int argc, char* argv[], char *errBuf, int errSize) {
    int ix = 0;
    char *key, *value;
    // begin parsing "--key value" args.
    // notice that quoted arg with spaces in between is already handled by shell
    if (errBuf && errSize > 0) errBuf[0] = '\0';
    if (argc == 1) return FALSE;
    for (;;) {
        if (++ix >= argc) break;
        key = argv[ix];
        if (key[0] != '-' || key[1] != '-' || key[2] == '\0') {
            if (errBuf && errSize > 0) {
                snprintf(errBuf, (size_t)errSize,
                         "'%.80s' is not an option. Every option is a \"--key value\" "
                         "pair, and a value containing spaces needs quotes.", key);
            }
            return FALSE;
        }
        key = &(key[2]); // skip "--"
        // A following "--something" is the next option, not this one's value:
        // nothing clumsy takes starts with two dashes, and reading it as a
        // value turns "--lag --lag-time 100" into a complaint about "100".
        if (++ix >= argc || (argv[ix][0] == '-' && argv[ix][1] == '-')) {
            if (errBuf && errSize > 0) {
                snprintf(errBuf, (size_t)errSize,
                         "--%.60s needs a value. Switches are spelled out too, "
                         "e.g. --%.60s on", key, key);
            }
            return FALSE;
        }
        value = argv[ix];
        argSet(key, value);
    }

    return TRUE;
}
