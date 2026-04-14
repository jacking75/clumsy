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

#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "common.h"

#define MAX_PROFILES        32
#define PROFILE_NAME_SIZE   48
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
    int depth;

    nProfiles = 0;

    buildFilePath(filePath, MSG_BUFSIZE);

    f = fopen(filePath, "r");
    if (!f) {
        LOG("profiles: '%s' not found — no profiles loaded", filePath);
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

    free(buf);
    LOG("profiles: loaded %d profiles from '%s'", nProfiles, filePath);
}

int profileApply(const char *name) {
    int i, j;
    for (i = 0; i < nProfiles; i++) {
        if (strcmp(profiles[i].name, name) == 0) {
            LOG("profiles: applying '%s' (%d params)", name, profiles[i].nParams);
            for (j = 0; j < profiles[i].nParams; j++) {
                if (!applyModuleKV(profiles[i].params[j].key,
                                   profiles[i].params[j].val)) {
                    LOG("profiles: WARNING unrecognized key '%s'",
                        profiles[i].params[j].key);
                }
            }
            return 1;
        }
    }
    LOG("profiles: profile '%s' not found", name);
    return 0;
}

int profileCount(void) {
    return nProfiles;
}

const char* profileGetName(int ix) {
    if (ix < 0 || ix >= nProfiles) return "";
    return profiles[ix].name;
}

// ---------------------------------------------------------------------------
// Write all profiles to profiles.json
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
    int i, ix;
    Profile *p = NULL;

    // find existing or allocate new slot
    for (i = 0; i < nProfiles; i++) {
        if (strcmp(profiles[i].name, name) == 0) {
            p = &profiles[i];
            break;
        }
    }
    if (!p) {
        if (nProfiles >= MAX_PROFILES) {
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
    LOG("profiles: saved '%s' with %d params", name, p->nParams);
    return 1;
}
