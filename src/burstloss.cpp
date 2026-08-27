// Burst packet loss module using Gilbert-Elliott 2-state Markov chain
//
// Two states:
//   GOOD - normal state, low drop probability (p)
//   BAD  - burst state,  high drop probability (q)
//
// Each packet:
//   1. Drop decision based on current state's probability
//   2. State transition based on p_gb (GOOD->BAD) or p_bg (BAD->GOOD)
//
// Average burst length  = 1 / p_bg
// Fraction of time BAD  = p_gb / (p_gb + p_bg)
// Overall loss rate     = p * (1 - fraction_bad) + q * fraction_bad

#include <stdlib.h>
#include <string.h>
#include "common.h"

#define NAME "burstloss"

// Default values in [0-10000] units (i.e. X/100 == X%)
#define DEFAULT_P_GOOD  200   //  2.0% drop in GOOD state
#define DEFAULT_P_BAD   8000  // 80.0% drop in BAD  state
#define DEFAULT_P_GB    500   //  5.0% GOOD -> BAD  transition per packet
#define DEFAULT_P_BG    2000  // 20.0% BAD  -> GOOD transition per packet

#define STATE_GOOD 0
#define STATE_BAD  1

static volatile short burstlossEnabled  = 0,
    burstlossInbound  = 1,
    burstlossOutbound = 1,
    chanceGood = DEFAULT_P_GOOD,
    chanceBad  = DEFAULT_P_BAD,
    chanceGB   = DEFAULT_P_GB,
    chanceBG   = DEFAULT_P_BG;

// state machine — only touched inside the divert mutex, so no volatile needed
static short currentState = STATE_GOOD;

static void burstlossStartUp() {
    currentState = STATE_GOOD;
    LOG("burstloss enabled, starting in GOOD state");
}

static void burstlossCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("burstloss disabled");
}

static short burstlossProcess(PacketNode *head, PacketNode *tail) {
    int dropped = 0;
    PacketNode *pac = head->next;

    // snapshot volatile params once per call for consistency across this packet batch
    short good = chanceGood, bad = chanceBad, gb = chanceGB, bg = chanceBG;

    while (pac != tail) {
        PacketNode *next = pac->next;

        if (checkDirection(pac->meta.outbound, burstlossInbound, burstlossOutbound)) {
            // 1. Drop decision based on current state
            short doDropped = (currentState == STATE_GOOD) ? calcChance(good)
                                                            : calcChance(bad);
            // 2. State transition
            if (currentState == STATE_GOOD) {
                if (calcChance(gb)) {
                    currentState = STATE_BAD;
                    LOG("burstloss: GOOD -> BAD (burst start)");
                }
            } else {
                if (calcChance(bg)) {
                    currentState = STATE_GOOD;
                    LOG("burstloss: BAD -> GOOD (burst end)");
                }
            }

            // 3. Apply drop
            if (doDropped) {
                LOG("burstloss: dropped in %s state", currentState == STATE_GOOD ? "GOOD" : "BAD");
                freeNode(popNode(pac));
                ++dropped;
                InterlockedIncrement(&burstlossModule.affectedCount);
            }
        }

        pac = next;
    }

    return dropped > 0;
}

static int burstlossSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&burstlossInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&burstlossOutbound, (short)parseBoolValue(value));
        return 1;
    }
    {
        short v = clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000);
        if (strcmp(key, NAME"-good") == 0) { InterlockedExchange16(&chanceGood, v); return 1; }
        if (strcmp(key, NAME"-bad")  == 0) { InterlockedExchange16(&chanceBad,  v); return 1; }
        if (strcmp(key, NAME"-gb")   == 0) { InterlockedExchange16(&chanceGB,   v); return 1; }
        if (strcmp(key, NAME"-bg")   == 0) { InterlockedExchange16(&chanceBG,   v); return 1; }
    }
    return 0;
}

static int burstlossGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 6) return 0;
    strcpy(kv[n].key, NAME"-good");
    sprintf(kv[n].val, "%.1f", (float)chanceGood / 100.0f); n++;
    strcpy(kv[n].key, NAME"-bad");
    sprintf(kv[n].val, "%.1f", (float)chanceBad / 100.0f); n++;
    strcpy(kv[n].key, NAME"-gb");
    sprintf(kv[n].val, "%.1f", (float)chanceGB / 100.0f); n++;
    strcpy(kv[n].key, NAME"-bg");
    sprintf(kv[n].val, "%.1f", (float)chanceBG / 100.0f); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, burstlossInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, burstlossOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec burstlossParamSpecs[] = {
    { NAME"-inbound",  "Inbound",              "bool",    0, 0 },
    { NAME"-outbound", "Outbound",             "bool",    0, 0 },
    { NAME"-good",     "Good drop (%)",        "percent", 0, 100 },
    { NAME"-bad",      "Bad drop (%)",         "percent", 0, 100 },
    { NAME"-gb",       "Good to Bad (%)",      "percent", 0, 100 },
    { NAME"-bg",       "Bad to Good (%)",      "percent", 0, 100 },
};

Module burstlossModule = {
    "Burst Loss",
    NAME,
    (short*)&burstlossEnabled,
    burstlossStartUp,
    burstlossCloseDown,
    burstlossProcess,
    burstlossSetParam,
    burstlossGetParams,
    burstlossParamSpecs,
    (int)(sizeof(burstlossParamSpecs) / sizeof(burstlossParamSpecs[0])),
    // runtime fields
    0, 0, 0
};
