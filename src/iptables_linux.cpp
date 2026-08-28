// Automatic iptables rule management  (Phase 4.3 second tier)
//
// With --auto-iptables, clumsy installs the NFQUEUE rules its filter needs and
// removes them again on shutdown, instead of making the operator hand-write and
// hand-clean them.
//
// This is the riskiest feature in the Linux port: a leftover NFQUEUE rule with
// no userspace program behind it silently drops every packet it matches. The
// design is built around never leaving one behind:
//
//   * every rule is installed with -I, and removed with the byte-identical -D
//     built from the same string, so removal cannot drift from installation
//   * only rules this process actually installed are removed, tracked in order
//   * removal runs from divertStop(), from the normal exit path, and from the
//     signal handler's path, so Ctrl+C is as safe as a clean stop
//   * every rule carries --queue-bypass, so even if cleanup is somehow missed
//     (SIGKILL, power loss) traffic flows normally once clumsy is gone
//
// That last point is the real safety net and is why auto rules are safer than
// the hand-written ones in the documentation.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "common.h"
#include "filterexpr.h"

#define MAX_INSTALLED 24
#define CMD_SIZE      512

namespace {

// The exact argument string used with -I, replayed verbatim with -D.
struct InstalledRule {
    char tool[16];      // "iptables" or "ip6tables"
    char chain[16];     // "OUTPUT" / "INPUT"
    char spec[320];     // everything after the chain name
};

InstalledRule installed[MAX_INSTALLED];
int           installedCount = 0;
int           autoEnabled    = 0;

// Runs one iptables invocation. Returns the exit status, or -1 if it could not
// be launched. Output is suppressed; the caller reports failures with context.
int runIptables(const char *tool, const char *op, const char *chain, const char *spec) {
    char cmd[CMD_SIZE];
    snprintf(cmd, sizeof(cmd), "%s %s %s %s >/dev/null 2>&1", tool, op, chain, spec);
    const int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

int recordAndInstall(const char *tool, const char *chain, const char *spec) {
    if (installedCount >= MAX_INSTALLED) {
        INFO("auto-iptables: rule limit reached, refusing to add more");
        return 0;
    }
    if (runIptables(tool, "-I", chain, spec) != 0) {
        INFO("auto-iptables: failed to install: %s -I %s %s", tool, chain, spec);
        return 0;
    }
    InstalledRule &r = installed[installedCount++];
    snprintf(r.tool,  sizeof(r.tool),  "%s", tool);
    snprintf(r.chain, sizeof(r.chain), "%s", chain);
    snprintf(r.spec,  sizeof(r.spec),  "%s", spec);
    LOG("auto-iptables: installed %s -I %s %s", tool, chain, spec);
    return 1;
}

} // namespace

// Declared here rather than in a header: only divert_linux.cpp calls these, and
// it declares them itself, so there is no need for an iptables_linux.h.
int  iptablesAutoInstall(const FilterProgram *prog, int queueNum, UINT32 injectMark);
void iptablesAutoRemove(void);
int  iptablesAutoIsEnabled(void);

int iptablesAutoIsEnabled(void) { return autoEnabled; }

// Installs the rules implied by the compiled filter. Returns 1 on success.
// Any partial failure rolls back, so we never leave half a rule set behind.
int iptablesAutoInstall(const FilterProgram *prog, int queueNum, UINT32 injectMark) {
    IptablesRule rules[MAX_DERIVED_RULES];
    int exact = 1;
    char spec[320];

    if (!parseBoolValue(argGet("auto-iptables"))) {
        autoEnabled = 0;
        return 1;   // not requested: not an error
    }

    const int count = filterDeriveIptables(prog, rules, MAX_DERIVED_RULES, &exact);
    if (count <= 0) {
        INFO("auto-iptables: could not derive any rule from the filter; "
             "install rules manually (see docs/LINUX.md).");
        return 0;
    }

    installedCount = 0;
    autoEnabled = 1;

    // The mark bypass must come first in the chain, so install it last: -I
    // always prepends. Getting this backwards would feed clumsy's own injected
    // packets straight back into the queue.
    for (int i = 0; i < count; ++i) {
        const char *tool = rules[i].ipv6 ? "ip6tables" : "iptables";

        // --queue-bypass is deliberate: if clumsy dies without cleaning up, the
        // kernel passes traffic through instead of blackholing it.
        snprintf(spec, sizeof(spec), "%s -j NFQUEUE --queue-num %d --queue-bypass",
                 rules[i].match, queueNum);

        if (rules[i].chains & IPT_CHAIN_OUTPUT) {
            if (!recordAndInstall(tool, "OUTPUT", spec)) goto rollback;
        }
        if (rules[i].chains & IPT_CHAIN_INPUT) {
            if (!recordAndInstall(tool, "INPUT", spec)) goto rollback;
        }
    }

    // Now the bypass, which ends up above everything installed above.
    snprintf(spec, sizeof(spec), "-m mark --mark 0x%x -j ACCEPT", injectMark);
    if (!recordAndInstall("iptables", "OUTPUT", spec)) goto rollback;

    INFO("auto-iptables: installed %d rule(s) for queue %d.", installedCount, queueNum);
    if (!exact) {
        INFO("auto-iptables: note - the generated rules are wider than the filter");
        INFO("               expression (some terms cannot be expressed in iptables).");
        INFO("               Extra traffic reaches clumsy and is passed through untouched.");
    }
    return 1;

rollback:
    INFO("auto-iptables: install failed, removing the rules added so far.");
    iptablesAutoRemove();
    autoEnabled = 0;
    return 0;
}

void iptablesAutoRemove(void) {
    if (installedCount == 0) return;

    // Reverse order so each -D sees the chain in the shape -I left it.
    int failures = 0;
    for (int i = installedCount - 1; i >= 0; --i) {
        const InstalledRule &r = installed[i];
        if (runIptables(r.tool, "-D", r.chain, r.spec) != 0) {
            ++failures;
            INFO("auto-iptables: FAILED to remove: %s -D %s %s",
                 r.tool, r.chain, r.spec);
        } else {
            LOG("auto-iptables: removed %s -D %s %s", r.tool, r.chain, r.spec);
        }
    }

    if (failures) {
        INFO("auto-iptables: %d rule(s) could not be removed - check with:", failures);
        INFO("               sudo iptables -L OUTPUT -n --line-numbers");
        INFO("               (they carry --queue-bypass, so traffic still flows)");
    } else {
        INFO("auto-iptables: removed %d rule(s).", installedCount);
    }

    installedCount = 0;
    autoEnabled = 0;
}
