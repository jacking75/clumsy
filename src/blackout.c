// Blackout module - simulates complete connection loss for a fixed duration,
// repeating periodically. Useful for testing client reconnect / timeout logic.
//
// Cycle:  [Normal: Gap ms] -> [Blackout: Dur ms] -> [Normal: Gap ms] -> ...
//
// The "Trigger" button immediately starts or cancels a blackout on demand,
// independent of the periodic cycle.

#include <stdlib.h>
#include "iup.h"
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

static Ihandle *inboundCheckbox, *outboundCheckbox;
static Ihandle *durInput, *gapInput, *triggerButton;

static volatile short blackoutEnabled   = 0,
    blackoutInbound  = 1,
    blackoutOutbound = 1,
    triggerPending   = 0;  // set by UI button, cleared in process()

static volatile LONG durationMs = BLACKOUT_DUR_DEFAULT;
static volatile LONG gapMs      = BLACKOUT_GAP_DEFAULT;

// state machine — only touched inside the divert mutex, no volatile needed
static short currentPhase = PHASE_NORMAL;
static DWORD phaseStart   = 0;

static int triggerBlackoutCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);
    if (!blackoutEnabled) return IUP_DEFAULT;
    InterlockedExchange16(&triggerPending, 1);
    return IUP_DEFAULT;
}

static Ihandle* blackoutSetupUI() {
    Ihandle *controlsBox = IupHbox(
        inboundCheckbox  = IupToggle("Inbound",  NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Dur(ms):"),
        durInput = IupText(NULL),
        IupLabel("Gap(ms):"),
        gapInput = IupText(NULL),
        triggerButton = IupButton("Trigger", NULL),
        NULL
    );

    // Blackout duration
    IupSetAttribute(durInput, "VISIBLECOLUMNS", "5");
    IupSetAttribute(durInput, "VALUE", STR(BLACKOUT_DUR_DEFAULT));
    IupSetCallback(durInput, "VALUECHANGED_CB", uiSyncInt32);
    IupSetAttribute(durInput, SYNCED_VALUE, (char*)&durationMs);
    IupSetAttribute(durInput, INTEGER_MAX, BLACKOUT_DUR_MAX);
    IupSetAttribute(durInput, INTEGER_MIN, BLACKOUT_DUR_MIN);

    // Normal-phase gap between blackouts
    IupSetAttribute(gapInput, "VISIBLECOLUMNS", "6");
    IupSetAttribute(gapInput, "VALUE", STR(BLACKOUT_GAP_DEFAULT));
    IupSetCallback(gapInput, "VALUECHANGED_CB", uiSyncInt32);
    IupSetAttribute(gapInput, SYNCED_VALUE, (char*)&gapMs);
    IupSetAttribute(gapInput, INTEGER_MAX, BLACKOUT_GAP_MAX);
    IupSetAttribute(gapInput, INTEGER_MIN, BLACKOUT_GAP_MIN);

    // Manual trigger button
    IupSetCallback(triggerButton, "ACTION", triggerBlackoutCb);
    IupSetAttribute(triggerButton, "PADDING", "4x");

    IupSetCallback(inboundCheckbox,  "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox,  SYNCED_VALUE, (char*)&blackoutInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&blackoutOutbound);

    IupSetAttribute(inboundCheckbox,  "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox,  "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(durInput, "VALUE", NAME"-duration");
        setFromParameter(gapInput, "VALUE", NAME"-gap");
    }

    return controlsBox;
}

static void blackoutStartUp() {
    currentPhase = PHASE_NORMAL;
    phaseStart   = timeGetTime();
    InterlockedExchange16(&triggerPending, 0);
    startTimePeriod();
    LOG("blackout enabled — gap=%ldms dur=%ldms", gapMs, durationMs);
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
            if (checkDirection(pac->addr.Outbound, blackoutInbound, blackoutOutbound)) {
                freeNode(popNode(pac));
                ++dropped;
                InterlockedIncrement(&blackoutModule.affectedCount);
            } else {
                head = head->next;
            }
        }
        return TRUE;  // keep icon active even when packet list is empty
    }

    return FALSE;
}

static int blackoutSetParam(const char *key, const char *value) {
    if (strcmp(key, "blackout-duration") == 0) {
        LONG v = atol(value);
        char buf[16];
        if (v < 100) v = 100; if (v > 60000) v = 60000;
        InterlockedExchange(&durationMs, v);
        sprintf(buf, "%ld", v);
        if (durInput) IupStoreAttribute(durInput, "VALUE", buf);
        return 1;
    }
    if (strcmp(key, "blackout-gap") == 0) {
        LONG v = atol(value);
        char buf[16];
        if (v < 1000) v = 1000; if (v > 300000) v = 300000;
        InterlockedExchange(&gapMs, v);
        sprintf(buf, "%ld", v);
        if (gapInput) IupStoreAttribute(gapInput, "VALUE", buf);
        return 1;
    }
    if (strcmp(key, "blackout-trigger") == 0) {
        if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
            InterlockedExchange16(&triggerPending, 1);
        }
        return 1;
    }
    return 0;
}

static int blackoutGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 2) return 0;
    strcpy(kv[n].key, "blackout-duration");
    sprintf(kv[n].val, "%ld", durationMs);
    n++;
    strcpy(kv[n].key, "blackout-gap");
    sprintf(kv[n].val, "%ld", gapMs);
    n++;
    return n;
}

Module blackoutModule = {
    "Blackout",
    NAME,
    (short*)&blackoutEnabled,
    blackoutSetupUI,
    blackoutStartUp,
    blackoutCloseDown,
    blackoutProcess,
    blackoutSetParam,
    blackoutGetParams,
    // runtime fields
    0, 0, NULL, 0
};
