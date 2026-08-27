// Blackout module - simulates complete connection loss for a fixed duration,
// repeating periodically. Useful for testing client reconnect / timeout logic.
//
// Cycle:  [Normal: Gap ms] -> [Blackout: Dur ms] -> [Normal: Gap ms] -> ...
//
// The "Trigger" button immediately starts or cancels a blackout on demand,
// independent of the periodic cycle.

#include <stdlib.h>
#include <string.h>
#include "common.h"

#define NAME "blackout"

#define BLACKOUT_DUR_DEFAULT  3000   // ms  — blackout duration
#define BLACKOUT_GAP_DEFAULT  15000  // ms  — normal gap between blackouts
#define BLACKOUT_DUR_MIN  "100"
#define BLACKOUT_DUR_MAX  "60000"
#define BLACKOUT_GAP_MIN  "1000"
#define BLACKOUT_GAP_MAX  "300000"

#define PHASE_NORMAL   0
#define PHASE_BLACKOUT 1

static volatile short blackoutEnabled   = 0,
    blackoutInbound  = 1,
    blackoutOutbound = 1,
    triggerPending   = 0;  // set by UI button, cleared in process()

static volatile LONG durationMs = BLACKOUT_DUR_DEFAULT;
static volatile LONG gapMs      = BLACKOUT_GAP_DEFAULT;

// state machine — only touched inside the divert mutex, no volatile needed
static short currentPhase = PHASE_NORMAL;
static DWORD phaseStart   = 0;

static void blackoutStartUp() {
    currentPhase = PHASE_NORMAL;
    phaseStart   = timeGetTime();
    InterlockedExchange16(&triggerPending, 0);
    startTimePeriod();
    LOG("blackout enabled - gap=%ldms dur=%ldms", gapMs, durationMs);
}

static void blackoutCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    currentPhase = PHASE_NORMAL;
    endTimePeriod();
    LOG("blackout disabled");
}

static short blackoutProcess(PacketNode *head, PacketNode *tail) {
    DWORD now = timeGetTime();
    int   dropped = 0;
    LONG  dur = durationMs, gap = gapMs;  // snapshot volatile params

    // handle manual trigger: toggles phase immediately
    if (triggerPending) {
        InterlockedExchange16(&triggerPending, 0);
        if (currentPhase == PHASE_NORMAL) {
            currentPhase = PHASE_BLACKOUT;
            LOG("blackout: manually triggered");
        } else {
            currentPhase = PHASE_NORMAL;
            LOG("blackout: manually cancelled");
        }
        phaseStart = now;
    }

    // periodic phase transition
    if (currentPhase == PHASE_NORMAL) {
        if ((DWORD)(now - phaseStart) >= (DWORD)gap) {
            currentPhase = PHASE_BLACKOUT;
            phaseStart   = now;
            LOG("blackout: started (periodic)");
        }
    } else { // PHASE_BLACKOUT
        if ((DWORD)(now - phaseStart) >= (DWORD)dur) {
            currentPhase = PHASE_NORMAL;
            phaseStart   = now;
            LOG("blackout: ended");
        }
    }

    // in BLACKOUT phase: drop all matching packets
    if (currentPhase == PHASE_BLACKOUT) {
        while (head->next != tail) {
            PacketNode *pac = head->next;
            if (checkDirection(pac->meta.outbound, blackoutInbound, blackoutOutbound)) {
                freeNode(popNode(pac));
                ++dropped;
                InterlockedIncrement(&blackoutModule.affectedCount);
            } else {
                head = head->next;
            }
        }
        LOG("blackout: dropped %d packets this step", dropped);
        return TRUE;  // stay marked active even when the packet list is empty
    }

    return FALSE;
}

static int blackoutSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-duration") == 0) {
        InterlockedExchange(&durationMs, clampLong(atol(value), 100, 60000));
        return 1;
    }
    if (strcmp(key, NAME"-gap") == 0) {
        InterlockedExchange(&gapMs, clampLong(atol(value), 1000, 300000));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&blackoutInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&blackoutOutbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-trigger") == 0) {
        // write-only trigger: toggles a blackout immediately on the next process()
        if (parseBoolValue(value) && blackoutEnabled) {
            InterlockedExchange16(&triggerPending, 1);
        }
        return 1;
    }
    return 0;
}

static int blackoutGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 4) return 0;
    strcpy(kv[n].key, NAME"-duration");
    sprintf(kv[n].val, "%ld", durationMs); n++;
    strcpy(kv[n].key, NAME"-gap");
    sprintf(kv[n].val, "%ld", gapMs); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, blackoutInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, blackoutOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec blackoutParamSpecs[] = {
    { NAME"-inbound",  "Inbound",       "bool",   0, 0 },
    { NAME"-outbound", "Outbound",      "bool",   0, 0 },
    { NAME"-duration", "Duration (ms)", "int",    100, 60000 },
    { NAME"-gap",      "Gap (ms)",      "int",    1000, 300000 },
    { NAME"-trigger",  "Trigger now",   "action", 0, 0 },
};

Module blackoutModule = {
    "Blackout",
    NAME,
    (short*)&blackoutEnabled,
    blackoutStartUp,
    blackoutCloseDown,
    blackoutProcess,
    blackoutSetParam,
    blackoutGetParams,
    blackoutParamSpecs,
    (int)(sizeof(blackoutParamSpecs) / sizeof(blackoutParamSpecs[0])),
    // runtime fields
    0, 0, 0
};
