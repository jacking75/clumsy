// Scenario scripting — time-based network condition changes
//
// A JSON array of steps, each with an "at" field (seconds from filtering start)
// and a set of module changes. Applied by the UI timer with ~200ms resolution.
//
// JSON format:
//   [
//     { "at": 0,  "lag": true,  "lag-time": 100 },
//     { "at": 10, "drop": true, "drop-chance": 5.0, "lag-time": 200 },
//     { "at": 30, "lag": false, "drop": false }
//   ]
//
// Keys:
//   "at"            — required, integer seconds from filtering start
//   "module-name"   — true/false to enable/disable that module
//   "param-key"     — parameter value, same keys as the Named Pipe API
//
// Thread safety: applyStep() uses InterlockedExchange16/InterlockedExchange
// for all writes; called from the main-thread UI timer (same pattern as UI toggles).

#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

#define SCENARIO_BUF_SIZE  65536  // max scenario file size (64 KB)
#define MAX_STEPS          128
#define MAX_PARAMS_PER_STEP 32
#define KEY_SIZE            48
#define VAL_SIZE            64

typedef struct {
    char k[KEY_SIZE];
    char v[VAL_SIZE];
} KvPair;

typedef struct {
    int    atSec;
    int    nParams;
    KvPair params[MAX_PARAMS_PER_STEP];
} ScenarioStep;

static ScenarioStep steps[MAX_STEPS];
static int          nSteps  = 0;
static int          cursor  = 0;    // index of next step to apply
static DWORD        startMs = 0;
static int          active  = 0;    // 1 while scenario is running

// ---------------------------------------------------------------------------
// Minimal JSON parser for a flat object
// ---------------------------------------------------------------------------

// Parse one {…} block; fills *step. Returns 1 on success (has valid "at"), 0 on failure.
static int parseObject(const char *p, const char *end, ScenarioStep *step) {
    char key[KEY_SIZE], val[VAL_SIZE];

    step->atSec  = -1;
    step->nParams = 0;

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
        } else if (step->nParams < MAX_PARAMS_PER_STEP) {
            memcpy(step->params[step->nParams].k, key, KEY_SIZE - 1);
            step->params[step->nParams].k[KEY_SIZE - 1] = '\0';
            memcpy(step->params[step->nParams].v, val, VAL_SIZE - 1);
            step->params[step->nParams].v[VAL_SIZE - 1] = '\0';
            step->nParams++;
        }
    }

    return step->atSec >= 0;
}

// Comparison function for insertion sort by atSec
static void sortSteps(void) {
    int i, j;
    for (i = 1; i < nSteps; i++) {
        ScenarioStep tmp = steps[i];
        for (j = i - 1; j >= 0 && steps[j].atSec > tmp.atSec; j--) {
            steps[j + 1] = steps[j];
        }
        steps[j + 1] = tmp;
    }
}

// ---------------------------------------------------------------------------
// Step application
// ---------------------------------------------------------------------------

static void applyStep(const ScenarioStep *step) {
    int i;
    LOG("scenario: step at=%ds (%d changes)", step->atSec, step->nParams);
    for (i = 0; i < step->nParams; i++) {
        if (!applyModuleKV(step->params[i].k, step->params[i].v)) {
            LOG("scenario: WARNING unrecognized key '%s'", step->params[i].k);
        }
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
        LOG("scenario: cannot open '%s'", path);
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
        LOG("scenario: no JSON array found in '%s'", path);
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
    LOG("scenario: loaded %d steps from '%s'", nSteps, path);
}

void scenarioStart(void) {
    if (nSteps == 0) return;
    cursor  = 0;
    startMs = GetTickCount();
    active  = 1;
    LOG("scenario: started (%d steps)", nSteps);
}

void scenarioStop(void) {
    active = 0;
    cursor = 0;
    LOG("scenario: stopped");
}

// Called from uiTimerCb (~200ms). Applies all steps whose "at" time has elapsed.
void scenarioTick(void) {
    DWORD elapsed;
    unsigned int elapsedSec;

    if (!active || cursor >= nSteps) return;

    elapsed    = GetTickCount() - startMs;
    elapsedSec = elapsed / 1000;

    while (cursor < nSteps && (unsigned int)steps[cursor].atSec <= elapsedSec) {
        applyStep(&steps[cursor]);
        cursor++;
    }

    // auto-stop when all steps applied
    if (cursor >= nSteps) {
        active = 0;
        LOG("scenario: all steps applied");
    }
}

int scenarioIsLoaded(void) {
    return nSteps > 0;
}
