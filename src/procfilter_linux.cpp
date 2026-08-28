// Process-based filter, Linux  (Phase 4.2)
//
// Same contract as procfilter.cpp on Windows: turn a process name into a filter
// fragment matching the local ports that process currently holds.
//
// Windows walks GetExtendedTcpTable/GetExtendedUdpTable. Linux has no such call,
// so we do what ss(8) does: collect the process's socket inodes from
// /proc/<pid>/fd, then map those inodes to local ports via /proc/net/{tcp,udp}.
//
// Same limitation as Windows: ports are a snapshot taken at Start. Connections
// the process opens afterwards are not covered.

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

#define MAX_PIDS  32
#define MAX_PORTS 64

namespace {

// Reads /proc/<pid>/comm, which holds the executable name (truncated to 15
// chars by the kernel, so compare accordingly).
bool processNameMatches(const char *pidDir, const char *wanted) {
    char path[512], name[256];
    snprintf(path, sizeof(path), "/proc/%s/comm", pidDir);

    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(name, sizeof(name), f)) { fclose(f); return false; }
    fclose(f);

    char *nl = strchr(name, '\n');
    if (nl) *nl = '\0';

    if (_stricmp(name, wanted) == 0) return true;

    // Accept "game" for a wanted name of "game.exe" so the same --process value
    // works on both platforms, and honour the kernel's 15-char truncation.
    const size_t nameLen = strlen(name);
    if (nameLen >= 15 && _strnicmp(name, wanted, nameLen) == 0) return true;

    const char *dot = strrchr(wanted, '.');
    if (dot && _strnicmp(name, wanted, (size_t)(dot - wanted)) == 0 &&
        strlen(name) == (size_t)(dot - wanted)) {
        return true;
    }
    return false;
}

bool isAllDigits(const char *s) {
    if (!*s) return false;
    for (; *s; ++s) if (!isdigit((unsigned char)*s)) return false;
    return true;
}

int findPidsByName(const char *name, long *pids, int maxPids) {
    DIR *proc = opendir("/proc");
    if (!proc) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(proc)) != nullptr && count < maxPids) {
        if (!isAllDigits(entry->d_name)) continue;
        if (processNameMatches(entry->d_name, name)) {
            pids[count++] = strtol(entry->d_name, nullptr, 10);
        }
    }
    closedir(proc);
    return count;
}

// Collects socket inodes owned by a pid. /proc/<pid>/fd/<n> is a symlink
// reading "socket:[12345]".
int collectSocketInodes(long pid, unsigned long *inodes, int maxInodes, int count) {
    char dirPath[256];
    snprintf(dirPath, sizeof(dirPath), "/proc/%ld/fd", pid);

    DIR *d = opendir(dirPath);
    if (!d) return count;   // process exited, or not ours to inspect

    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr && count < maxInodes) {
        char linkPath[512], target[256];
        snprintf(linkPath, sizeof(linkPath), "%s/%s", dirPath, entry->d_name);
        const ssize_t n = readlink(linkPath, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        if (strncmp(target, "socket:[", 8) == 0) {
            inodes[count++] = strtoul(target + 8, nullptr, 10);
        }
    }
    closedir(d);
    return count;
}

bool inodeWanted(unsigned long inode, const unsigned long *inodes, int count) {
    for (int i = 0; i < count; ++i) if (inodes[i] == inode) return true;
    return false;
}

bool portAlreadyKnown(unsigned port, const unsigned *ports, int count) {
    for (int i = 0; i < count; ++i) if (ports[i] == port) return true;
    return false;
}

// Scans one of /proc/net/{tcp,tcp6,udp,udp6}, appending local ports whose
// socket inode belongs to the target process.
int scanProcNet(const char *path, const unsigned long *inodes, int inodeCount,
                unsigned *ports, int maxPorts, int portCount) {
    FILE *f = fopen(path, "r");
    if (!f) return portCount;

    char line[512];
    if (!fgets(line, sizeof(line), f)) { fclose(f); return portCount; }  // header

    while (fgets(line, sizeof(line), f) && portCount < maxPorts) {
        char localAddr[128];
        unsigned long inode = 0;
        // sl local_address rem_address st tx:rx tr:when retrnsmt uid timeout inode
        if (sscanf(line, "%*d: %127s %*s %*x %*x:%*x %*x:%*x %*x %*u %*u %lu",
                   localAddr, &inode) != 2) {
            continue;
        }
        if (inode == 0 || !inodeWanted(inode, inodes, inodeCount)) continue;

        const char *colon = strrchr(localAddr, ':');
        if (!colon) continue;
        const unsigned port = (unsigned)strtoul(colon + 1, nullptr, 16);
        if (port == 0 || portAlreadyKnown(port, ports, portCount)) continue;
        ports[portCount++] = port;
    }
    fclose(f);
    return portCount;
}

} // namespace

int buildProcessFilter(const char *processName, char *filterBuf, int bufSize,
                       char *errBuf, int errSize) {
    long pids[MAX_PIDS];
    unsigned long inodes[512];
    unsigned ports[MAX_PORTS];
    int pos = 0;

    if (!processName || processName[0] == '\0') return 0;
    if (filterBuf) filterBuf[0] = '\0';

    const int pidCount = findPidsByName(processName, pids, MAX_PIDS);
    if (pidCount == 0) {
        snprintf(errBuf, (size_t)errSize,
                 "Process '%s' is not running.", processName);
        return -1;
    }

    int inodeCount = 0;
    for (int i = 0; i < pidCount; ++i) {
        inodeCount = collectSocketInodes(pids[i], inodes,
                                         (int)(sizeof(inodes) / sizeof(inodes[0])),
                                         inodeCount);
    }
    if (inodeCount == 0) {
        snprintf(errBuf, (size_t)errSize,
                 "Found %d process(es) named '%s' but could not read their sockets. "
                 "Run clumsy as root to inspect other users' processes.",
                 pidCount, processName);
        return -1;
    }

    int portCount = 0;
    portCount = scanProcNet("/proc/net/tcp",  inodes, inodeCount, ports, MAX_PORTS, portCount);
    portCount = scanProcNet("/proc/net/tcp6", inodes, inodeCount, ports, MAX_PORTS, portCount);
    portCount = scanProcNet("/proc/net/udp",  inodes, inodeCount, ports, MAX_PORTS, portCount);
    portCount = scanProcNet("/proc/net/udp6", inodes, inodeCount, ports, MAX_PORTS, portCount);

    if (portCount == 0) {
        snprintf(errBuf, (size_t)errSize,
                 "Process '%s' is running but has no active network ports.",
                 processName);
        return -1;
    }

    // Same fragment shape the Windows build produces, so the combined filter
    // string is identical on both platforms.
    //
    // appendf() clamps, which matters here: one fragment runs to about 100
    // bytes with five-digit ports, so accumulating raw snprintf return values
    // against a 64-byte margin could leave pos past the end of the buffer, and
    // the unguarded closing ")" would then be written outside the caller's
    // stack array. The margin is 120 to match the Windows side as well.
    pos = appendf(filterBuf, bufSize, pos, " and (");
    for (int i = 0; i < portCount; ++i) {
        if (pos >= bufSize - 120) {
            LOG("procfilter: filter truncated at %d/%d ports", i, portCount);
            break;
        }
        pos = appendf(filterBuf, bufSize, pos,
                      "%stcp.SrcPort == %u or tcp.DstPort == %u or "
                      "udp.SrcPort == %u or udp.DstPort == %u",
                      i ? " or " : "", ports[i], ports[i], ports[i], ports[i]);
    }
    appendf(filterBuf, bufSize, pos, ")");

    return portCount;
}
