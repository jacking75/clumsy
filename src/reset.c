// Reset injection packet module
#include <stdlib.h>
#include "iup.h"
#include "common.h"
#define NAME "reset"

static const unsigned int TCP_MIN_SIZE = sizeof(WINDIVERT_IPHDR) + sizeof(WINDIVERT_TCPHDR);

static Ihandle *inboundCheckbox, *outboundCheckbox, *chanceInput, *rstButton;

static volatile short resetEnabled = 0,
    resetInbound = 1, resetOutbound = 1,
    chance = 0, // [0-10000]
    setNextCount = 0;


static int resetSetRSTNextButtonCb(Ihandle *ih) {
    UNREFERENCED_PARAMETER(ih);

    if (!(*resetModule.enabledFlag)) {
        return IUP_DEFAULT;
    }

    InterlockedIncrement16(&setNextCount);

    return IUP_DEFAULT;
}

static Ihandle* resetSetupUI() {
    Ihandle *dupControlsBox = IupHbox(
        rstButton = IupButton("RST next packet", NULL),
        inboundCheckbox = IupToggle("Inbound", NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Chance(%):"),
        chanceInput = IupText(NULL),
        NULL
        );

    IupSetAttribute(chanceInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(chanceInput, "VALUE", "0");
    IupSetCallback(chanceInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(chanceInput, SYNCED_VALUE, (char*)&chance);
    IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char*)&resetInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&resetOutbound);
    IupSetCallback(rstButton, "ACTION", resetSetRSTNextButtonCb);
    IupSetAttribute(rstButton, "PADDING", "4x");

    // enable by default to avoid confusing
    IupSetAttribute(inboundCheckbox, "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox, "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(chanceInput, "VALUE", NAME"-chance");
    }

    return dupControlsBox;
}

static void resetStartup() {
    LOG("reset enabled");
    InterlockedExchange16(&setNextCount, 0);
}

static void resetCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("reset disabled");
    InterlockedExchange16(&setNextCount, 0);
}

static short resetProcess(PacketNode *head, PacketNode *tail) {
    short reset = FALSE;
    PacketNode *pac = head->next;
    while (pac != tail) {
        if (checkDirection(pac->addr.Outbound, resetInbound, resetOutbound)
            && pac->packetLen > TCP_MIN_SIZE
            && (setNextCount || calcChance(chance)))
        {
            PWINDIVERT_TCPHDR pTcpHdr;
            WinDivertHelperParsePacket(
                pac->packet,
                pac->packetLen,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                &pTcpHdr,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL);

            if (pTcpHdr != NULL) {
                LOG("injecting reset w/ chance %.1f%%", chance/100.0);
                pTcpHdr->Rst = 1;
                WinDivertHelperCalcChecksums(pac->packet, pac->packetLen, NULL, 0);

                InterlockedIncrement(&resetModule.affectedCount);
                reset = TRUE;
                if (setNextCount > 0) {
                    InterlockedDecrement16(&setNextCount);
                }
            }
        }
        
        pac = pac->next;
    }
    return reset;
}

static int resetSetParam(const char *key, const char *value) {
    if (strcmp(key, "reset-chance") == 0) {
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

static int resetGetParams(ParamKV *kv, int maxKv) {
    if (maxKv < 1) return 0;
    strcpy(kv[0].key, "reset-chance");
    sprintf(kv[0].val, "%.1f", (float)chance / 100.0f);
    return 1;
}

Module resetModule = {
    "Set TCP RST",
    NAME,
    (short*)&resetEnabled,
    resetSetupUI,
    resetStartup,
    resetCloseDown,
    resetProcess,
    resetSetParam,
    resetGetParams,
    // runtime fields
    0, 0, NULL, 0
};