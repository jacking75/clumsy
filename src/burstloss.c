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
#include "iup.h"
#include "common.h"

#define NAME "burstloss"

// Default values in [0-10000] units (i.e. X/100 == X%)
#define DEFAULT_P_GOOD  200   //  2.0% drop in GOOD state
#define DEFAULT_P_BAD   8000  // 80.0% drop in BAD  state
#define DEFAULT_P_GB    500   //  5.0% GOOD -> BAD  transition per packet
#define DEFAULT_P_BG    2000  // 20.0% BAD  -> GOOD transition per packet

#define STATE_GOOD 0
#define STATE_BAD  1

static Ihandle *inboundCheckbox, *outboundCheckbox;
static Ihandle *goodInput, *badInput, *gbInput, *bgInput;

static volatile short burstlossEnabled  = 0,
    burstlossInbound  = 1,
    burstlossOutbound = 1,
    chanceGood = DEFAULT_P_GOOD,
    chanceBad  = DEFAULT_P_BAD,
    chanceGB   = DEFAULT_P_GB,
    chanceBG   = DEFAULT_P_BG;

// state machine — only touched inside the divert mutex, so no volatile needed
static short currentState = STATE_GOOD;

static Ihandle* burstlossSetupUI() {
    Ihandle *controlsBox = IupHbox(
        inboundCheckbox  = IupToggle("Inbound",  NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Good(%):"),
        goodInput = IupText(NULL),
        IupLabel("Bad(%):"),
        badInput  = IupText(NULL),
        IupLabel("G>B(%):"),
        gbInput   = IupText(NULL),
        IupLabel("B>G(%):"),
        bgInput   = IupText(NULL),
        NULL
    );

    // Good state drop chance
    IupSetAttribute(goodInput, "VISIBLECOLUMNS", "3");
    IupSetAttribute(goodInput, "VALUE", "2.0");
    IupSetCallback(goodInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(goodInput, SYNCED_VALUE, (char*)&chanceGood);

    // Bad state drop chance
    IupSetAttribute(badInput, "VISIBLECOLUMNS", "3");
    IupSetAttribute(badInput, "VALUE", "80.0");
    IupSetCallback(badInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(badInput, SYNCED_VALUE, (char*)&chanceBad);

    // GOOD -> BAD transition probability per packet
    IupSetAttribute(gbInput, "VISIBLECOLUMNS", "3");
    IupSetAttribute(gbInput, "VALUE", "5.0");
    IupSetCallback(gbInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(gbInput, SYNCED_VALUE, (char*)&chanceGB);

    // BAD -> GOOD transition probability per packet
    IupSetAttribute(bgInput, "VISIBLECOLUMNS", "3");
    IupSetAttribute(bgInput, "VALUE", "20.0");
    IupSetCallback(bgInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(bgInput, SYNCED_VALUE, (char*)&chanceBG);

    IupSetCallback(inboundCheckbox,  "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox,  SYNCED_VALUE, (char*)&burstlossInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&burstlossOutbound);

    IupSetAttribute(inboundCheckbox,  "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox,  "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(goodInput, "VALUE", NAME"-good");
        setFromParameter(badInput,  "VALUE", NAME"-bad");
        setFromParameter(gbInput,   "VALUE", NAME"-gb");
        setFromParameter(bgInput,   "VALUE", NAME"-bg");
    }

    return controlsBox;
}

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

        if (checkDirection(pac->addr.Outbound, burstlossInbound, burstlossOutbound)) {
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
    short v = I2S((int)(atof(value) * 100.0 + 0.5));
    char buf[16];
    if (v < 0) v = 0; if (v > 10000) v = 10000;
    sprintf(buf, "%.1f", (float)v / 100.0f);
    if (strcmp(key, "burstloss-good") == 0) { InterlockedExchange16(&chanceGood, v); if (goodInput) IupStoreAttribute(goodInput, "VALUE", buf); return 1; }
    if (strcmp(key, "burstloss-bad")  == 0) { InterlockedExchange16(&chanceBad,  v); if (badInput)  IupStoreAttribute(badInput,  "VALUE", buf); return 1; }
    if (strcmp(key, "burstloss-gb")   == 0) { InterlockedExchange16(&chanceGB,   v); if (gbInput)   IupStoreAttribute(gbInput,   "VALUE", buf); return 1; }
    if (strcmp(key, "burstloss-bg")   == 0) { InterlockedExchange16(&chanceBG,   v); if (bgInput)   IupStoreAttribute(bgInput,   "VALUE", buf); return 1; }
    return 0;
}

static int burstlossGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 4) return 0;
    strcpy(kv[n].key, "burstloss-good");
    sprintf(kv[n].val, "%.1f", (float)chanceGood / 100.0f);
    n++;
    strcpy(kv[n].key, "burstloss-bad");
    sprintf(kv[n].val, "%.1f", (float)chanceBad / 100.0f);
    n++;
    strcpy(kv[n].key, "burstloss-gb");
    sprintf(kv[n].val, "%.1f", (float)chanceGB / 100.0f);
    n++;
    strcpy(kv[n].key, "burstloss-bg");
    sprintf(kv[n].val, "%.1f", (float)chanceBG / 100.0f);
    n++;
    return n;
}

Module burstlossModule = {
    "Burst Loss",
    NAME,
    (short*)&burstlossEnabled,
    burstlossSetupUI,
    burstlossStartUp,
    burstlossCloseDown,
    burstlossProcess,
    burstlossSetParam,
    burstlossGetParams,
    // runtime fields
    0, 0, NULL, 0
};
