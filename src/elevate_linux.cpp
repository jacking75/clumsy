// Privilege checks, Linux  (Phase 4.4)
//
// The Windows build asks "are we elevated?" and can relaunch itself through
// UAC. Linux has no equivalent prompt, and re-executing under sudo from inside
// a running process is worse than telling the operator what to do, so
// tryElevate() here only explains.
//
// What actually matters is CAP_NET_ADMIN: NFQUEUE needs it, and a process can
// hold it without being root (file capabilities on the binary, or a container
// granting it). Checking euid alone would wrongly reject those setups, so we
// read the effective capability set from /proc/self/status.

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

namespace {

// Bit numbers from <linux/capability.h>, hardcoded so libcap is not a
// build dependency for a two-bit check.
constexpr int CAP_NET_ADMIN_BIT = 12;
constexpr int CAP_NET_RAW_BIT   = 13;

// Reads CapEff from /proc/self/status. Returns 0 when it cannot be read.
unsigned long long readEffectiveCaps(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;

    char line[256];
    unsigned long long caps = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "CapEff:", 7) == 0) {
            caps = strtoull(line + 7, nullptr, 16);
            break;
        }
    }
    fclose(f);
    return caps;
}

bool hasCapability(int bit) {
    return (readEffectiveCaps() >> bit) & 1ULL;
}

} // namespace

BOOL IsRunAsAdmin() {
    // "Can this process open an NFQUEUE?" is the question every caller is really
    // asking, so answer that rather than "is euid 0".
    if (geteuid() == 0) return TRUE;
    return hasCapability(CAP_NET_ADMIN_BIT) ? TRUE : FALSE;
}

BOOL IsElevated() {
    return IsRunAsAdmin();
}

BOOL tryElevate(BOOL silent) {
    if (IsRunAsAdmin()) return FALSE;   // nothing to do, keep running

    if (!silent) {
        INFO("clumsy needs CAP_NET_ADMIN to open an NFQUEUE.");
        INFO("Either run it under sudo:");
        INFO("    sudo clumsy --filter \"udp and outbound\"");
        INFO("or grant the binary the capabilities once:");
        INFO("    sudo setcap cap_net_admin,cap_net_raw+ep ./clumsy");
        if (!hasCapability(CAP_NET_RAW_BIT)) {
            INFO("(CAP_NET_RAW is also needed for the duplicate module's raw socket.)");
        }
    }
    // Unlike Windows there is no way to re-launch elevated without a terminal
    // prompt, so the caller keeps running in limited mode and the web dashboard
    // still comes up - same behaviour as a non-elevated Windows run.
    return FALSE;
}
