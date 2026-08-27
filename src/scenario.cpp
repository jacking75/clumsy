// Scenario scripting — timed and conditional network condition changes
//
// A JSON array of steps. Each step carries exactly one trigger:
//
//   time trigger        { "at": 10, "lag": true, "lag-time": 200 }
//   condition trigger   { "when": "captured_count", "op": ">=", "value": 10000,
//                         "drop": true }
//
// and optionally repeats:
//
//   { "at": 0, "repeat": 30, "times": 5, "drop-chance": 5.0 }
//
// Keys:
//   "at"          — integer seconds from filtering start (time trigger)
//   "when"        — metric name (condition trigger). One of:
//                     captured_count, sent_count, pps,
//                     affected:<module>, elapsed_sec,
//                     lag_buf, jitter_buf, bandwidth_buf
//   "op"          — one of >=, >, <=, <, ==  (default ">=")
//   "value"       — number compared against the metric
//   "repeat"      — re-arm the step every N seconds after it fires
//   "times"       — how many times a repeating step may fire (0/absent = forever)
//   "module-name" — true/false to enable/disable that module
//   "param-key"   — parameter value, same keys as the control API
//
// Time-based steps keep their original semantics exactly: they are sorted by
// "at" and applied in order once their second has elapsed.
//
// Thread safety: applyStep() writes through applyModuleKV(), which uses the
// Interlocked* helpers. scenarioTick() is called from the main loop only.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

#define SCENARIO_BUF_SIZE  65536  // max scenario file size (64 KB)
#define MAX_STEPS          128
#define MAX_PARAMS_PER_STEP 32
#define KEY_SIZE            48
#define VAL_SIZE            64

#define TRIGGER_TIME      0
#define TRIGGER_CONDITION 1

#define OP_GE 0
#define OP_GT 1
#define OP_LE 2
#define OP_LT 3
#define OP_EQ 4

typedef struct {
    char k[KEY_SIZE];
    char v[VAL_SIZE];
} KvPair;

typedef struct {
    int    triggerType;
    int    atSec;                 // TRIGGER_TIME
    char   whenMetric[KEY_SIZE];  // TRIGGER_CONDITION
    int    op;
    double value;
    int    repeatSec;             // 0 = fire once
    int    maxTimes;              // 0 = unlimited (only with repeatSec)
    int    nParams;
    KvPair params[MAX_PARAMS_PER_STEP];
    // runtime
    int    fireCount;
    DWORD  nextFireMs;            // for repeating steps
    int    done;
} ScenarioStep;

static ScenarioStep steps[MAX_STEPS];
static int          nSteps  = 0;
static DWORD        startMs = 0;
static int          active  = 0;    // 1 while scenario is running
static LONG         prevCaptured = 0;
static DWORD        prevPpsMs = 0;
static double       lastPps = 0.0;

// ---------------------------------------------------------------------------
// Minimal JSON parser for a flat object
// ---------------------------------------------------------------------------

static int parseOp(const char *s) {
    if (strcmp(s, ">")  == 0) return OP_GT;
    if (strcmp(s, "<=") == 0) return OP_LE;
    if (strcmp(s, "<")  == 0) return OP_LT;
    if (strcmp(s, "==") == 0 || strcmp(s, "=") == 0) return OP_EQ;
    return OP_GE;
}

// Parse one {…} block; fills *step. Returns 1 when the step has a usable
// trigger (either "at" or "when"), 0 otherwise.
static int parseObject(const char *p, const char *end, ScenarioStep *step) {
    char key[KEY_SIZE], val[VAL_SIZE];
    int hasAt = 0, hasWhen = 0;

    memset(step, 0, sizeof(*step));
    step->atSec = -1;
    step->op    = OP_GE;

    while (p < end) {
        const char *keyEnd, *valEnd, *valStart;
        int klen, vlen;

        // skip whitespace and commas
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
        if (p >= end || *p == '}') break;
        if (*p != '"') { p++; continue; }
        p++;

        // read key
        keyEnd = p;
        while (keyEnd < end && *keyEnd != '"') keyEnd++;
        if (keyEnd >= end) break;
        klen = (int)(keyEnd - p);
        if (klen <= 0 || klen >= KEY_SIZE) { p = keyEnd + 1; continue; }
        memcpy(key, p, klen); key[klen] = '\0';
        p = keyEnd + 1;

        // skip colon and whitespace
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
        if (p >= end) break;

        // read value
        if (*p == '"') {
            p++;
            valEnd = p;
            while (valEnd < end && *valEnd != '"') valEnd++;
            vlen = (int)(valEnd - p);
            if (vlen >= VAL_SIZE) vlen = VAL_SIZE - 1;
            memcpy(val, p, vlen); val[vlen] = '\0';
            p = (valEnd < end) ? valEnd + 1 : end;
        } else {
            valStart = p;
            while (p < end && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            vlen = (int)(p - valStart);
            if (vlen >= VAL_SIZE) vlen = VAL_SIZE - 1;
            memcpy(val, valStart, vlen); val[vlen] = '\0';
        }

        if (strcmp(key, "at") == 0) {
            step->atSec = atoi(val);
            hasAt = 1;
        } else if (strcmp(key, "when") == 0) {
            // snprintf rather than strncpy: val is the larger buffer, and this
            // always terminates.
            snprintf(step->whenMetric, KEY_SIZE, "%s", val);
            hasWhen = 1;
        } else if (strcmp(key, "op") == 0) {
            step->op = parseOp(val);
        } else if (strcmp(key, "value") == 0) {
            step->value = atof(val);
        } else if (strcmp(key, "repeat") == 0) {
            step->repeatSec = atoi(val);
        } else if (strcmp(key, "times") == 0) {
            step->maxTimes = atoi(val);
        } else if (step->nParams < MAX_PARAMS_PER_STEP) {
            memcpy(step->params[step->nParams].k, key, KEY_SIZE - 1);
            step->params[step->nParams].k[KEY_SIZE - 1] = '\0';
            memcpy(step->params[step->nParams].v, val, VAL_SIZE - 1);
            step->params[step->nParams].v[VAL_SIZE - 1] = '\0';
            step->nParams++;
        }
    }

    // Each step owns exactly one trigger type. "when" wins if both appear, so a
    // malformed step still behaves predictably instead of firing twice.
    if (hasWhen) {
        step->triggerType = TRIGGER_CONDITION;
        return 1;
    }
    if (hasAt && step->atSec >= 0) {
        step->triggerType = TRIGGER_TIME;
        return 1;
    }
    return 0;
}

// Insertion sort by atSec. Condition steps sort to the end and keep their file
// order; they are evaluated every tick anyway, so relative order among them
// only matters when two fire on the same tick.
static void sortSteps(void) {
    int i, j;
    for (i = 1; i < nSteps; i++) {
        ScenarioStep tmp = steps[i];
        int tmpKey = (tmp.triggerType == TRIGGER_TIME) ? tmp.atSec : 0x7FFFFFFF;
        for (j = i - 1; j >= 0; j--) {
            int jKey = (steps[j].triggerType == TRIGGER_TIME) ? steps[j].atSec : 0x7FFFFFFF;
            if (jKey <= tmpKey) break;
            steps[j + 1] = steps[j];
        }
        steps[j + 1] = tmp;
    }
}

// ---------------------------------------------------------------------------
// Metrics available to condition triggers
// ---------------------------------------------------------------------------

static double readMetric(const char *name, DWORD elapsedMs) {
    if (strcmp(name, "captured_count") == 0) return (double)statsCapturedTotal;
    if (strcmp(name, "sent_count")     == 0) return (double)statsSentTotal;
    if (strcmp(name, "elapsed_sec")    == 0) return (double)elapsedMs / 1000.0;
    if (strcmp(name, "pps")            == 0) return lastPps;
    if (strcmp(name, "lag_buf")        == 0) return (double)lagGetBufSize();
    if (strcmp(name, "jitter_buf")     == 0) return (double)jitterGetBufSize();
    if (strcmp(name, "bandwidth_buf")  == 0) return (double)bandwidthGetBufSize();
    if (strncmp(name, "affected:", 9) == 0) {
        const char *mod = name + 9;
        for (int ix = 0; ix < MODULE_CNT; ++ix) {
            if (strcmp(modules[ix]->shortName, mod) == 0) {
                return (double)modules[ix]->affectedCount;
            }
        }
    }
    return 0.0;
}

static int compareOp(double lhs, int op, double rhs) {
    switch (op) {
    case OP_GT: return lhs >  rhs;
    case OP_LE: return lhs <= rhs;
    case OP_LT: return lhs <  rhs;
    case OP_EQ: return lhs == rhs;
    default:    return lhs >= rhs;
    }
}

static const char* opName(int op) {
    switch (op) {
    case OP_GT: return ">";
    case OP_LE: return "<=";
    case OP_LT: return "<";
    case OP_EQ: return "==";
    default:    return ">=";
    }
}

// ---------------------------------------------------------------------------
// Step application
// ---------------------------------------------------------------------------

static void applyStep(ScenarioStep *step) {
    char note[192];
    int i;

    if (step->triggerType == TRIGGER_TIME) {
        LOG("scenario: step at=%ds (%d changes)", step->atSec, step->nParams);
        snprintf(note, sizeof(note), "scenario step at %ds", step->atSec);
    } else {
        LOG("scenario: condition %s %s %.2f met (%d changes)",
            step->whenMetric, opName(step->op), step->value, step->nParams);
        snprintf(note, sizeof(note), "scenario condition %s %s %g",
                 step->whenMetric, opName(step->op), step->value);
    }
    reportNoteEvent(note);

    for (i = 0; i < step->nParams; i++) {
        if (!applyModuleKV(step->params[i].k, step->params[i].v)) {
            INFO("scenario: WARNING unrecognized key '%s'", step->params[i].k);
        }
    }
    step->fireCount++;
}

// Marks a step as consumed, or re-arms it when it repeats.
static void retireStep(ScenarioStep *step, DWORD nowMs) {
    if (step->repeatSec > 0 &&
        (step->maxTimes <= 0 || step->fireCount < step->maxTimes)) {
        step->nextFireMs = nowMs + (DWORD)step->repeatSec * 1000;
    } else {
        step->done = 1;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void scenarioLoad(const char *path) {
    FILE *f;
    char *buf;
    size_t len;
    const char *p;
    int depth;

    nSteps = 0;

    f = fopen(path, "r");
    if (!f) {
        INFO("scenario: cannot open '%s'", path);
        return;
    }

    buf = (char*)malloc(SCENARIO_BUF_SIZE);
    if (!buf) { fclose(f); return; }

    len = fread(buf, 1, SCENARIO_BUF_SIZE - 1, f);
    fclose(f);
    buf[len] = '\0';

    // find opening '['
    p = strchr(buf, '[');
    if (!p) {
        INFO("scenario: no JSON array found in '%s'", path);
        free(buf);
        return;
    }
    p++;

    // extract and parse each '{...}' object
    while (*p && nSteps < MAX_STEPS) {
        const char *objStart;

        while (*p && *p != '{' && *p != ']') p++;
        if (!*p || *p == ']') break;

        objStart = p + 1; // content starts after '{'
        depth = 1;
        p++;
        while (*p && depth > 0) {
            if      (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        // p now points one past the matching '}'

        if (parseObject(objStart, p - 1, &steps[nSteps])) {
            nSteps++;
        }
    }

    free(buf);
    sortSteps();
    INFO("scenario: loaded %d steps from '%s'", nSteps, path);
}

void scenarioStart(void) {
    int i;
    if (nSteps == 0) return;
    startMs = GetTickCount();
    active  = 1;
    prevCaptured = statsCapturedTotal;
    prevPpsMs = startMs;
    lastPps = 0.0;
    for (i = 0; i < nSteps; i++) {
        steps[i].fireCount  = 0;
        steps[i].done       = 0;
        steps[i].nextFireMs = 0;
    }
    INFO("scenario: started (%d steps)", nSteps);
}

void scenarioStop(void) {
    if (active) LOG("scenario: stopped");
    active = 0;
}

// Called from the main tick loop (~200ms) while capturing.
void scenarioTick(void) {
    DWORD now, elapsed;
    unsigned int elapsedSec;
    int i, remaining = 0;

    if (!active || nSteps == 0) return;

    now        = GetTickCount();
    elapsed    = now - startMs;
    elapsedSec = elapsed / 1000;

    // refresh the derived pps metric roughly once a second
    if ((now - prevPpsMs) >= 1000) {
        LONG captured = statsCapturedTotal;
        lastPps = (double)(captured - prevCaptured) * 1000.0 / (double)(now - prevPpsMs);
        prevCaptured = captured;
        prevPpsMs = now;
    }

    for (i = 0; i < nSteps; i++) {
        ScenarioStep *step = &steps[i];
        int fire = 0;

        if (step->done) continue;
        remaining++;

        if (step->nextFireMs != 0) {
            // repeating step waiting to re-arm
            if (now >= step->nextFireMs) fire = 1;
        } else if (step->triggerType == TRIGGER_TIME) {
            if ((unsigned int)step->atSec <= elapsedSec) fire = 1;
        } else {
            if (compareOp(readMetric(step->whenMetric, elapsed), step->op, step->value)) {
                fire = 1;
            }
        }

        if (fire) {
            applyStep(step);
            retireStep(step, now);
            if (step->done) remaining--;
        }
    }

    // auto-stop once every step is spent
    if (remaining == 0) {
        active = 0;
        INFO("scenario: all steps applied");
    }
}

int scenarioIsLoaded(void)  { return nSteps > 0; }
int scenarioStepCount(void) { return nSteps; }
int scenarioIsActive(void)  { return active; }
