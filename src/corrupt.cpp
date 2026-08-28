// corrupt module - bit error injection (realistic wireless corruption)
//
// tamper.cpp rewrites a whole slice of the payload with an XOR pattern, which
// models a deliberate man-in-the-middle. This models the other thing: the
// low-rate independent bit flips a radio link produces (Wi-Fi, cellular,
// satellite). With the checksum redone the corruption reaches the application;
// with it left alone the receiving stack discards the packet the way a real
// link-layer CRC failure would.
//
// Bit errors are drawn by gap rather than by trial. The obvious loop asks
// rand() once per bit, which is 11200 calls for a 1400-byte payload and would
// stall the packet pipeline at any real rate. The gap between two independent
// bit errors is geometric, so drawing the gap directly costs one rand() per
// error actually injected - two or three per packet at the default rate.

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

#define NAME "corrupt"

static volatile short corruptEnabled = 0,
    corruptInbound  = 1,
    corruptOutbound = 1,
    corruptChance   = 1000,   // [0-10000], 1000 = 10% of packets
    corruptChecksum = 1;      // recompute checksums so corruption reaches the app

// Per-bit error probability in parts per million. 100ppm = 0.01%, which puts
// roughly one flipped bit in every 1250 bytes.
static volatile LONG corruptBer = 100;

// Cumulative bits flipped, reported through the parameter map so an operator
// can confirm the rate actually applied.
static volatile LONG corruptBitsFlipped = 0;

static void corruptStartUp() {
    LOG("corrupt enabled");
}

static void corruptCloseDown(PacketNode *head, PacketNode *tail) {
    UNREFERENCED_PARAMETER(head);
    UNREFERENCED_PARAMETER(tail);
    LOG("corrupt closed down");
}

// Uniform in (0,1). The half-step keeps log() away from an exact zero.
static INLINE_FUNCTION double corruptUrand(void) {
    return ((double)rand() + 0.5) / ((double)RAND_MAX + 1.0);
}

// Flips bits in [data, data+len) at the configured rate. Returns how many.
static int flipBits(char *data, UINT len, LONG ppm) {
    double p, logq;
    unsigned long totalBits, bit = 0;
    int flipped = 0;

    if (len == 0 || ppm <= 0) return 0;

    p = (double)ppm / 1000000.0;
    if (p >= 1.0) {
        // Every bit flips; no point drawing gaps.
        for (UINT i = 0; i < len; ++i) data[i] = (char)(~(unsigned char)data[i]);
        return (int)(len * 8);
    }

    logq = log(1.0 - p);          // negative
    totalBits = (unsigned long)len * 8u;

    for (;;) {
        // Geometric gap to the next error, in bits.
        double gap = log(corruptUrand()) / logq;
        if (!(gap >= 0.0) || gap >= (double)totalBits) break;   // also catches NaN
        bit += (unsigned long)gap;
        if (bit >= totalBits) break;
        data[bit >> 3] ^= (char)(1u << (bit & 7u));
        ++flipped;
        ++bit;
    }
    return flipped;
}

static short corruptProcess(PacketNode *head, PacketNode *tail) {
    short did = FALSE;
    PacketNode *pac = head->next;
    LONG ppm = corruptBer;        // snapshot the volatile once per call

    while (pac != tail) {
        if (checkDirection(pac->meta.outbound, corruptInbound, corruptOutbound)
            && calcChance(corruptChance)) {
            char *data = NULL;
            UINT dataLen = 0;
            if (packetGetPayload(pac->packet, pac->packetLen, &data, &dataLen) && dataLen) {
                int flipped = flipBits(data, dataLen, ppm);
                if (flipped > 0) {
                    if (corruptChecksum) {
                        packetRecalcChecksums(pac->packet, pac->packetLen);
                    }
                    InterlockedExchangeAdd(&corruptBitsFlipped, (LONG)flipped);
                    InterlockedIncrement(&corruptModule.affectedCount);
                    did = TRUE;
                    LOG("corrupt flipped %d bit(s) in a %u byte payload", flipped, dataLen);
                }
            }
        }
        pac = pac->next;
    }
    return did;
}

static int corruptSetParam(const char *key, const char *value) {
    if (strcmp(key, NAME"-chance") == 0) {
        InterlockedExchange16(&corruptChance,
                              clampShort((int)(atof(value) * 100.0 + 0.5), 0, 10000));
        return 1;
    }
    if (strcmp(key, NAME"-ber") == 0) {
        InterlockedExchange(&corruptBer, clampLong((LONG)atol(value), 0, 1000000));
        return 1;
    }
    if (strcmp(key, NAME"-checksum") == 0) {
        InterlockedExchange16(&corruptChecksum, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-inbound") == 0) {
        InterlockedExchange16(&corruptInbound, (short)parseBoolValue(value));
        return 1;
    }
    if (strcmp(key, NAME"-outbound") == 0) {
        InterlockedExchange16(&corruptOutbound, (short)parseBoolValue(value));
        return 1;
    }
    return 0;
}

static int corruptGetParams(ParamKV *kv, int maxKv) {
    int n = 0;
    if (maxKv < 5) return 0;
    strcpy(kv[n].key, NAME"-chance");
    sprintf(kv[n].val, "%.1f", (double)corruptChance / 100.0); n++;
    strcpy(kv[n].key, NAME"-ber");
    sprintf(kv[n].val, "%ld", (long)corruptBer); n++;
    strcpy(kv[n].key, NAME"-checksum");
    strcpy(kv[n].val, corruptChecksum ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-inbound");
    strcpy(kv[n].val, corruptInbound ? "true" : "false"); n++;
    strcpy(kv[n].key, NAME"-outbound");
    strcpy(kv[n].val, corruptOutbound ? "true" : "false"); n++;
    return n;
}

static const ParamSpec corruptParamSpecs[] = {
    { NAME"-inbound",  "Inbound",         "bool",    0, 0,       NULL },
    { NAME"-outbound", "Outbound",        "bool",    0, 0,       NULL },
    { NAME"-checksum", "Redo checksum",   "bool",    0, 0,       NULL },
    { NAME"-chance",   "Chance (%)",      "percent", 0, 100,     NULL },
    { NAME"-ber",      "Bit error (ppm)", "int",     0, 1000000, NULL },
};

LONG corruptGetBitsFlipped(void) { return corruptBitsFlipped; }

Module corruptModule = {
    "Corrupt",
    NAME,
    (short*)&corruptEnabled,
    corruptStartUp,
    corruptCloseDown,
    corruptProcess,
    corruptSetParam,
    corruptGetParams,
    corruptParamSpecs,
    (int)(sizeof(corruptParamSpecs) / sizeof(corruptParamSpecs[0])),
    // runtime fields
    0, 0, 0
};
