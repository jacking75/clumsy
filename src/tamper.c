// tampering packet module
//
// Tampers 1/4 of the packet payload using XOR patterns.
// Position of the tampered region is selectable:
//   Front  (0) — first quarter of the payload
//   Center (1) — middle quarter (original behavior)
//   Back   (2) — last quarter of the payload
//   Random (3) — random offset each time
//
// Short packets (<=4 bytes) always have the entire payload tampered.

#include <stdlib.h>
#include "iup.h"
#include "windivert.h"
#include "common.h"

#define NAME "tamper"

#define POS_FRONT   0
#define POS_CENTER  1
#define POS_BACK    2
#define POS_RANDOM  3

static Ihandle *inboundCheckbox, *outboundCheckbox, *chanceInput,
               *checksumCheckbox, *positionList;

static volatile short tamperEnabled = 0,
    tamperInbound  = 1,
    tamperOutbound = 1,
    chance         = 1000, // [0-10000]
    doChecksum     = 1,    // recompute checksum after tampering
    tamperPos      = POS_CENTER;

// IupList ACTION: item is 1-indexed, maps directly to POS_* (0-indexed)
static int syncPositionList(Ihandle *ih, char *text, int item, int state) {
    UNREFERENCED_PARAMETER(ih);
    UNREFERENCED_PARAMETER(text);
    if (state == 1) {
        tamperPos = (short)(item - 1);
    }
    return IUP_DEFAULT;
}

static Ihandle* tamperSetupUI() {
    Ihandle *dupControlsBox = IupHbox(
        checksumCheckbox = IupToggle("Redo Checksum", NULL),
        inboundCheckbox  = IupToggle("Inbound",  NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Chance(%):"),
        chanceInput  = IupText(NULL),
        IupLabel("Position:"),
        positionList = IupList(NULL),
        NULL
    );

    IupSetAttribute(chanceInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(chanceInput, "VALUE", "10.0");
    IupSetCallback(chanceInput, "VALUECHANGED_CB", uiSyncChance);
    IupSetAttribute(chanceInput, SYNCED_VALUE, (char*)&chance);

    IupSetCallback(inboundCheckbox,  "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox,  SYNCED_VALUE, (char*)&tamperInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&tamperOutbound);
    IupSetCallback(checksumCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(checksumCheckbox, SYNCED_VALUE, (char*)&doChecksum);

    // position dropdown: 1=Front, 2=Center, 3=Back, 4=Random
    IupSetAttribute(positionList, "DROPDOWN", "YES");
    IupSetAttribute(positionList, "1", "Front");
    IupSetAttribute(positionList, "2", "Center");
    IupSetAttribute(positionList, "3", "Back");
    IupSetAttribute(positionList, "4", "Random");
    IupSetAttribute(positionList, "VALUE", "2"); // default: Center
    IupSetCallback(positionList, "ACTION", (Icallback)syncPositionList);

    // enable by default
    IupSetAttribute(inboundCheckbox,  "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");
    IupSetAttribute(checksumCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox,  "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(chanceInput,      "VALUE", NAME"-chance");
        setFromParameter(checksumCheckbox, "VALUE", NAME"-checksum");
        setFromParameter(positionList,     "VALUE", NAME"-position");
        // IupList ACTION only fires on user clicks; sync tamperPos manually
        {
            const char *val = IupGetAttribute(positionList, "VALUE");
            if (val) {
                short pos = (short)(atoi(val) - 1);
                if (pos >= POS_FRONT && pos <= POS_RANDOM) tamperPos = pos;
            }
        }
    }

    return dupControlsBox;
}

// patterns covers every bit combination
#define PATTERN_CNT 8
static char patterns[] = {
    0x64,
    0x13,
    0x88,

    0x40,
    0x1F,
    0xA0,

    0xAA,
    0x55
};
static int patIx;

static void tamperStartup() {
    patIx = 0;
    LOG("tamper enabled, pos=%d", tamperPos);
}

static void tamperCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("tamper disabled");
}

static INLINE_FUNCTION void tamper_buf(char *buf, UINT len) {
    UINT ix;
    for (ix = 0; ix < len; ++ix) {
        buf[ix] ^= patterns[patIx++ & 0x7];
    }
}

static short tamperProcess(PacketNode *head, PacketNode *tail) {
    short tampered = FALSE;
    PacketNode *pac = head->next;
    while (pac != tail) {
        if (checkDirection(pac->addr.Outbound, tamperInbound, tamperOutbound)
            && calcChance(chance)) {
            char *data  = NULL;
            UINT dataLen = 0;
            if (WinDivertHelperParsePacket(pac->packet, pac->packetLen,
                    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                    (PVOID*)&data, &dataLen, NULL, NULL)
                && data != NULL && dataLen != 0) {

                if (dataLen <= 4) {
                    // short packet: tamper everything
                    tamper_buf(data, dataLen);
                    LOG("tampered short packet (%u bytes)", dataLen);
                } else {
                    UINT len_d4 = dataLen / 4;
                    UINT offset;
                    short pos = tamperPos; // snapshot volatile

                    switch (pos) {
                    case POS_FRONT:
                        offset = 0;
                        break;
                    case POS_BACK:
                        offset = dataLen - len_d4;
                        break;
                    case POS_RANDOM:
                        offset = (UINT)(rand() % (int)(dataLen - len_d4 + 1));
                        break;
                    default: // POS_CENTER
                        offset = dataLen / 2 - len_d4 / 2 + 1;
                        break;
                    }

                    tamper_buf(data + offset, len_d4);
                    LOG("tampered pos=%d offset=%u len_d4=%u dataLen=%u",
                        pos, offset, len_d4, dataLen);
                }

                if (doChecksum) {
                    WinDivertHelperCalcChecksums(pac->packet, pac->packetLen, NULL, 0);
                }
                InterlockedIncrement(&tamperModule.affectedCount);
                tampered = TRUE;
            }
        }
        pac = pac->next;
    }
    return tampered;
}

static int tamperSetParam(const char *key, const char *value) {
    if (strcmp(key, "tamper-chance") == 0) {
        short v = I2S((int)(atof(value) * 100.0 + 0.5));
        char buf[16];
        if (v < 0) v = 0; if (v > 10000) v = 10000;
        InterlockedExchange16(&chance, v);
        sprintf(buf, "%.1f", (float)v / 100.0f);
        if (chanceInput) IupStoreAttribute(chanceInput, "VALUE", buf);
        return 1;
    }
    if (strcmp(key, "tamper-position") == 0) {
        int v = atoi(value);
        char buf[4];
        if (v < POS_FRONT) v = POS_FRONT;
        if (v > POS_RANDOM) v = POS_RANDOM;
        InterlockedExchange16(&tamperPos, I2S(v));
        sprintf(buf, "%d", v + 1); // IupList is 1-indexed
        if (positionList) IupSetAttribute(positionList, "VALUE", buf);
        return 1;
    }
    return 0;
}

static int tamperGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 2) return 0;
    strcpy(kv[n].key, "tamper-chance");
    sprintf(kv[n].val, "%.1f", (float)chance / 100.0f);
    n++;
    strcpy(kv[n].key, "tamper-position");
    sprintf(kv[n].val, "%d", (int)tamperPos);
    n++;
    return n;
}

Module tamperModule = {
    "Tamper",
    NAME,
    (short*)&tamperEnabled,
    tamperSetupUI,
    tamperStartup,
    tamperCloseDown,
    tamperProcess,
    tamperSetParam,
    tamperGetParams,
    // runtime fields
    0, 0, NULL, 0
};
