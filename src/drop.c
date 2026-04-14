// dropping packet module
#include <stdlib.h>
#include <Windows.h>
#include "iup.h"
#include "common.h"
#define NAME "drop"

static Ihandle *inboundCheckbox, *outboundCheckbox, *chanceInput;

static volatile short dropEnabled = 0,
    dropInbound = 1, dropOutbound = 1,
    chance = 1000; // [0-10000]


static Ihandle* dropSetupUI() {
    Ihandle *dropControlsBox = IupHbox(
        inboundCheckbox = IupToggle("Inbound", NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Chance(%):"),
        chanceInput = IupText(NULL),
        NULL
    );

    IupSetAttribute(chanceInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(chanceInput, "VALUE", "10.0");
    IupSetCallback(chanceInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(chanceInput, SYNCED_VALUE, (char*)&chance);
    IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char*)&dropInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&dropOutbound);

    // enable by default to avoid confusing
    IupSetAttribute(inboundCheckbox, "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox, "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(chanceInput, "VALUE", NAME"-chance");
    }

    return dropControlsBox;
}

static void dropStartUp() {
    LOG("drop enabled");
}

static void dropCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("drop disabled");
}

static short dropProcess(PacketNode *head, PacketNode* tail) {
    int dropped = 0;
    while (head->next != tail) {
        PacketNode *pac = head->next;
        // chance in range of [0, 10000]
        if (checkDirection(pac->addr.Outbound, dropInbound, dropOutbound)
            && calcChance(chance)) {
            LOG("dropped with chance %.1f%%, direction %s",
                chance/100.0, pac->addr.Outbound ? "OUTBOUND" : "INBOUND");
            freeNode(popNode(pac));
            ++dropped;
            InterlockedIncrement(&dropModule.affectedCount);
        } else {
            head = head->next;
        }
    }

    return dropped > 0;
}


static int dropSetParam(const char *key, const char *value) {
    if (strcmp(key, "drop-chance") == 0) {
        short v = I2S((int)(atof(value) * 100.0 + 0.5));
        char buf[16];
        if (v < 0) v = 0; if (v > 10000) v = 10000;
        InterlockedExchange16(&chance, v);
        sprintf(buf, "%.1f", (float)v / 100.0f);
        if (chanceInput) IupStoreAttribute(chanceInput, "VALUE", buf);
        return 1;
    }
    return 0;
}

static int dropGetParams(ParamKV *kv, int maxKv) {
    if (maxKv < 1) return 0;
    strcpy(kv[0].key, "drop-chance");
    sprintf(kv[0].val, "%.1f", (float)chance / 100.0f);
    return 1;
}

Module dropModule = {
    "Drop",
    NAME,
    (short*)&dropEnabled,
    dropSetupUI,
    dropStartUp,
    dropCloseDown,
    dropProcess,
    dropSetParam,
    dropGetParams,
    // runtime fields
    0, 0, NULL, 0
};
