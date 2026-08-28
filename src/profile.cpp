// Profile save/load — named module configuration presets
//
// Loads profiles.json from the same directory as the executable.
// A profile is a flat JSON object of module enable/param changes,
// applied in one shot (no time component, unlike scenario steps).
//
// profiles.json format:
//   {
//     "mobile-4g": {
//       "lag": true,  "lag-time": 80,
//       "drop": true, "drop-chance": 2.0,
//       "bandwidth": true, "bandwidth-bandwidth": 5000
//     },
//     "bad-wifi": {
//       "lag": true, "lag-time": 50,
//       "jitter": true, "jitter-min": 20, "jitter-max": 200,
//       "burstloss": true
//     }
//   }
//
// Apply via CLI:  --profile mobile-4g
// Apply via UI:   Profile dropdown in the top bar
//
// Threading: every entry point below is reachable from an HTTP or Named Pipe
// worker (GET /api/profiles, POST /api/profiles/{name}/save|delete), so the
// array is guarded by profileLock in the same way pcapexport.cpp guards its
// state with pcapLock. Without it a delete shifting the array down could make
// a listing in flight skip an entry or return the same one twice.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

#define MAX_PROFILE_PARAMS  32
#define PROFILES_BUF_SIZE   65536
#define PROFILES_FILENAME   "profiles.json"

typedef struct {
    char    name[PROFILE_NAME_SIZE];
    int     nParams;
    ParamKV params[MAX_PROFILE_PARAMS];
} Profile;

static Profile profiles[MAX_PROFILES];
static int     nProfiles = 0;

static CRITICAL_SECTION profileLock;
static volatile short   profileLockReady = 0;

void profileInit(void) {
    if (profileLockReady) return;
    InitializeCriticalSection(&profileLock);
    profileLockReady = 1;
}

// profilesLoad() and profileApply() also run from argument handling, before
// profileInit() has had a chance to run in a unit-test harness; guarding here
// keeps every call site from having to care.
static INLINE_FUNCTION void profileLockEnter(void) {
    if (profileLockReady) EnterCriticalSection(&profileLock);
}
static INLINE_FUNCTION void profileLockLeave(void) {
    if (profileLockReady) LeaveCriticalSection(&profileLock);
}

// ---------------------------------------------------------------------------
// Minimal JSON parser — flat inner object
// ---------------------------------------------------------------------------

// Parse key:value pairs from [p, end). Fill kv[] up to maxKv entries.
// Returns number of pairs parsed.
static int parseInnerObject(const char *p, const char *end,
                            ParamKV *kv, int maxKv) {
    int n = 0;

    while (p < end && n < maxKv) {
        const char *keyEnd, *valEnd, *valStart;
        int klen, vlen;

        while (p < end && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n'||*p==',')) p++;
        if (p >= end || *p == '}') break;
        if (*p != '"') { p++; continue; }
        p++;

        keyEnd = p;
        while (keyEnd < end && *keyEnd != '"') keyEnd++;
        if (keyEnd >= end) break;
        klen = (int)(keyEnd - p);
        if (klen <= 0 || klen >= PARAM_KEY_SIZE) { p = keyEnd + 1; continue; }
        memcpy(kv[n].key, p, klen); kv[n].key[klen] = '\0';
        p = keyEnd + 1;

        while (p < end && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n'||*p==':')) p++;
        if (p >= end) break;

        if (*p == '"') {
            p++;
            valEnd = p;
            while (valEnd < end && *valEnd != '"') valEnd++;
            vlen = (int)(valEnd - p);
            if (vlen >= PARAM_VAL_SIZE) vlen = PARAM_VAL_SIZE - 1;
            memcpy(kv[n].val, p, vlen); kv[n].val[vlen] = '\0';
            p = (valEnd < end) ? valEnd + 1 : end;
        } else {
            valStart = p;
            while (p < end && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            vlen = (int)(p - valStart);
            if (vlen >= PARAM_VAL_SIZE) vlen = PARAM_VAL_SIZE - 1;
            memcpy(kv[n].val, valStart, vlen); kv[n].val[vlen] = '\0';
        }

        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Build the path to profiles.json (same directory as executable)
// ---------------------------------------------------------------------------
static void buildFilePath(char *out, int outSize) {
    char exePath[MSG_BUFSIZE];
    char *sep;
    (void)outSize;
    GetModuleFileNameA(NULL, exePath, MSG_BUFSIZE);
    sep = strrchr(exePath, '\\');
    if (!sep) sep = strrchr(exePath, '/');
    if (sep) {
        int dirLen = (int)(sep - exePath + 1);
        memcpy(out, exePath, dirLen);
        strcpy(out + dirLen, PROFILES_FILENAME);
    } else {
        strcpy(out, PROFILES_FILENAME);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void profilesLoad(void) {
    char  filePath[MSG_BUFSIZE];
    char *buf;
    FILE *f;
    size_t len;
    const char *p;
    int depth, count = 0;

    buildFilePath(filePath, MSG_BUFSIZE);

    f = fopen(filePath, "r");
    if (!f) {
        LOG("profiles: '%s' not found - no profiles loaded", filePath);
        profileLockEnter();
        nProfiles = 0;
        profileLockLeave();
        return;
    }

    buf = (char *)malloc(PROFILES_BUF_SIZE);
    if (!buf) { fclose(f); return; }

    len = fread(buf, 1, PROFILES_BUF_SIZE - 1, f);
    fclose(f);
    buf[len] = '\0';

    // find outer '{'
    p = strchr(buf, '{');
    if (!p) { free(buf); return; }
    p++;

    // The array is emptied and refilled as one operation so a listing never
    // observes the half-built state in between.
    profileLockEnter();
    nProfiles = 0;

    // iterate: "name" : { ... }
    while (*p && nProfiles < MAX_PROFILES) {
        const char *nameStart, *nameEnd, *objStart;
        int nameLen;

        // skip whitespace and commas
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') { p++; continue; }
        p++;

        // read profile name
        nameStart = p;
        nameEnd   = strchr(p, '"');
        if (!nameEnd) break;
        nameLen = (int)(nameEnd - nameStart);
        if (nameLen <= 0 || nameLen >= PROFILE_NAME_SIZE) { p = nameEnd + 1; continue; }

        memcpy(profiles[nProfiles].name, nameStart, nameLen);
        profiles[nProfiles].name[nameLen] = '\0';
        p = nameEnd + 1;

        // skip : whitespace
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
        if (*p != '{') continue;

        // find matching '}'
        objStart = p + 1;
        depth = 1;
        p++;
        while (*p && depth > 0) {
            if      (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }

        profiles[nProfiles].nParams =
            parseInnerObject(objStart, p - 1,
                             profiles[nProfiles].params, MAX_PROFILE_PARAMS);
        nProfiles++;
    }
    count = nProfiles;
    profileLockLeave();

    free(buf);
    LOG("profiles: loaded %d profiles from '%s'", count, filePath);
}

int profileApply(const char *name) {
    // Zero-initialised so the "not found" path leaves nothing readable behind;
    // it also spares the compiler having to prove `found` guards every use.
    Profile copy = {};
    int i, j, found = 0;

    // Copy the profile out, then apply it with the lock released:
    // applyModuleKV() reaches into every module and there is no reason to hold
    // a delete request off for the length of that walk.
    profileLockEnter();
    for (i = 0; i < nProfiles; i++) {
        if (strcmp(profiles[i].name, name) == 0) {
            copy  = profiles[i];
            found = 1;
            break;
        }
    }
    profileLockLeave();

    if (!found) {
        LOG("profiles: profile '%s' not found", name);
        return 0;
    }

    LOG("profiles: applying '%s' (%d params)", name, copy.nParams);
    for (j = 0; j < copy.nParams; j++) {
        if (!applyModuleKV(copy.params[j].key, copy.params[j].val)) {
            LOG("profiles: WARNING unrecognized key '%s'", copy.params[j].key);
        }
    }
    return 1;
}

static void profilesWriteAll(void);

// Removes a profile and rewrites profiles.json. Returns 0 when the name is
// unknown, so the caller can answer 404 rather than pretending it worked.
int profileDelete(const char *name) {
    int i, j, found = 0;

    profileLockEnter();
    for (i = 0; i < nProfiles; i++) {
        if (strcmp(profiles[i].name, name) == 0) {
            for (j = i; j < nProfiles - 1; j++) profiles[j] = profiles[j + 1];
            nProfiles--;
            // Rewritten while still holding the lock, so the file on disk
            // always matches the array a concurrent save would have seen.
            profilesWriteAll();
            found = 1;
            break;
        }
    }
    profileLockLeave();

    if (found) {
        INFO("profiles: deleted '%s'", name);
        return 1;
    }
    LOG("profiles: cannot delete '%s' - not found", name);
    return 0;
}

int profileCount(void) {
    int n;
    profileLockEnter();
    n = nProfiles;
    profileLockLeave();
    return n;
}

// One snapshot instead of count()-then-getName(i): between those two calls a
// delete can shift the array, and the caller would list an entry twice or miss
// one entirely.
int profileNamesSnapshot(char names[][PROFILE_NAME_SIZE], int maxNames) {
    int i, n;

    if (!names || maxNames <= 0) return 0;
    profileLockEnter();
    n = (nProfiles < maxNames) ? nProfiles : maxNames;
    for (i = 0; i < n; i++) {
        memcpy(names[i], profiles[i].name, PROFILE_NAME_SIZE);
        names[i][PROFILE_NAME_SIZE - 1] = '\0';
    }
    profileLockLeave();
    return n;
}

// ---------------------------------------------------------------------------
// Write all profiles to profiles.json. Callers hold profileLock.
// ---------------------------------------------------------------------------
static void profilesWriteAll(void) {
    char filePath[MSG_BUFSIZE];
    FILE *f;
    int i, j;

    buildFilePath(filePath, MSG_BUFSIZE);
    f = fopen(filePath, "w");
    if (!f) {
        LOG("profiles: failed to open '%s' for writing", filePath);
        return;
    }

    fprintf(f, "{\n");
    for (i = 0; i < nProfiles; i++) {
        fprintf(f, "  \"%s\": {\n", profiles[i].name);
        for (j = 0; j < profiles[i].nParams; j++) {
            fprintf(f, "    \"%s\": %s%s\n",
                    profiles[i].params[j].key,
                    profiles[i].params[j].val,
                    (j < profiles[i].nParams - 1) ? "," : "");
        }
        fprintf(f, "  }%s\n", (i < nProfiles - 1) ? "," : "");
    }
    fprintf(f, "}\n");

    fclose(f);
    LOG("profiles: wrote %d profiles to '%s'", nProfiles, filePath);
}

// ---------------------------------------------------------------------------
// Save current module state as a named profile
// ---------------------------------------------------------------------------
int profileSaveCurrent(const char *name) {
    int i, ix, nParams;
    Profile *p = NULL;

    profileLockEnter();

    // find existing or allocate new slot
    for (i = 0; i < nProfiles; i++) {
        if (strcmp(profiles[i].name, name) == 0) {
            p = &profiles[i];
            break;
        }
    }
    if (!p) {
        if (nProfiles >= MAX_PROFILES) {
            profileLockLeave();
            LOG("profiles: max profile count reached, cannot save '%s'", name);
            return 0;
        }
        p = &profiles[nProfiles];
        nProfiles++;
    }

    strncpy(p->name, name, PROFILE_NAME_SIZE - 1);
    p->name[PROFILE_NAME_SIZE - 1] = '\0';
    p->nParams = 0;

    // collect enabled modules and their parameters
    for (i = 0; i < MODULE_CNT; i++) {
        short enabled = *(modules[i]->enabledFlag);
        if (enabled) {
            // write enable flag
            ix = p->nParams;
            if (ix >= MAX_PROFILE_PARAMS) break;
            strncpy(p->params[ix].key, modules[i]->shortName, PARAM_KEY_SIZE - 1);
            strcpy(p->params[ix].val, "true");
            p->nParams++;

            // write parameters
            if (modules[i]->getParams) {
                int cnt = modules[i]->getParams(
                    &p->params[p->nParams],
                    MAX_PROFILE_PARAMS - p->nParams);
                p->nParams += cnt;
            }
        }
    }

    profilesWriteAll();
    nParams = p->nParams;
    profileLockLeave();

    LOG("profiles: saved '%s' with %d params", name, nParams);
    return 1;
}
