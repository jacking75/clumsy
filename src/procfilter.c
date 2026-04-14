// procfilter.c - resolve process name to WinDivert port-based filter
//
// WinDivert NETWORK layer does not support processId in filters, so we:
//   1. Resolve process name -> PID(s)  via CreateToolhelp32Snapshot
//   2. Enumerate local ports for those PIDs via GetExtendedTcp/UdpTable
//   3. Build a port-matching filter fragment to AND with the user filter
//
// Limitation: ports are snapshotted at Start time. If the process opens new
// connections after filtering starts, those won't be captured. For typical
// game sessions (connect then test) this is sufficient.

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <iphlpapi.h>
#include "common.h"

#define MAX_PIDS  32
#define MAX_PORTS 64

// ---- internal helpers ----

static int findPidsByName(const char *name, DWORD *pids, int maxPids) {
    HANDLE snap;
    PROCESSENTRY32 pe;
    int count = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0 && count < maxPids) {
                pids[count++] = pe.th32ProcessID;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return count;
}

static int isPidMatch(DWORD pid, const DWORD *pids, int pidCount) {
    int i;
    for (i = 0; i < pidCount; i++) {
        if (pids[i] == pid) return 1;
    }
    return 0;
}

static int isPortDup(WORD port, const WORD *ports, int count) {
    int i;
    for (i = 0; i < count; i++) {
        if (ports[i] == port) return 1;
    }
    return 0;
}

static int collectPorts(const DWORD *pids, int pidCount,
                        WORD *ports, int maxPorts) {
    int portCount = 0;
    DWORD size;

    // ---- TCP (IPv4) ----
    size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
        PMIB_TCPTABLE_OWNER_PID t = (PMIB_TCPTABLE_OWNER_PID)malloc(size);
        if (t && GetExtendedTcpTable(t, &size, FALSE, AF_INET,
                                     TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount)) {
                    WORD p = ntohs((WORD)t->table[i].dwLocalPort);
                    if (p != 0 && !isPortDup(p, ports, portCount))
                        ports[portCount++] = p;
                }
            }
        }
        free(t);
    }

    // ---- UDP (IPv4) ----
    size = 0;
    if (GetExtendedUdpTable(NULL, &size, FALSE, AF_INET,
                            UDP_TABLE_OWNER_PID, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
        PMIB_UDPTABLE_OWNER_PID t = (PMIB_UDPTABLE_OWNER_PID)malloc(size);
        if (t && GetExtendedUdpTable(t, &size, FALSE, AF_INET,
                                     UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount)) {
                    WORD p = ntohs((WORD)t->table[i].dwLocalPort);
                    if (p != 0 && !isPortDup(p, ports, portCount))
                        ports[portCount++] = p;
                }
            }
        }
        free(t);
    }

    // ---- TCP (IPv6) ----
    size = 0;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET6,
                            TCP_TABLE_OWNER_PID_ALL, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
        PMIB_TCP6TABLE_OWNER_PID t = (PMIB_TCP6TABLE_OWNER_PID)malloc(size);
        if (t && GetExtendedTcpTable(t, &size, FALSE, AF_INET6,
                                     TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount)) {
                    WORD p = ntohs((WORD)t->table[i].dwLocalPort);
                    if (p != 0 && !isPortDup(p, ports, portCount))
                        ports[portCount++] = p;
                }
            }
        }
        free(t);
    }

    // ---- UDP (IPv6) ----
    size = 0;
    if (GetExtendedUdpTable(NULL, &size, FALSE, AF_INET6,
                            UDP_TABLE_OWNER_PID, 0)
            == ERROR_INSUFFICIENT_BUFFER) {
        PMIB_UDP6TABLE_OWNER_PID t = (PMIB_UDP6TABLE_OWNER_PID)malloc(size);
        if (t && GetExtendedUdpTable(t, &size, FALSE, AF_INET6,
                                     UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount)) {
                    WORD p = ntohs((WORD)t->table[i].dwLocalPort);
                    if (p != 0 && !isPortDup(p, ports, portCount))
                        ports[portCount++] = p;
                }
            }
        }
        free(t);
    }

    return portCount;
}

// ---- public API ----

int buildProcessFilter(const char *processName, char *filterBuf, int bufSize,
                       char *errBuf, int errSize) {
    DWORD pids[MAX_PIDS];
    WORD  ports[MAX_PORTS];
    int   pidCount, portCount, i, pos;

    if (!processName || processName[0] == '\0') return 0;

    pidCount = findPidsByName(processName, pids, MAX_PIDS);
    if (pidCount == 0) {
        if (errBuf)
            snprintf(errBuf, errSize,
                "Process '%s' not found. Make sure it is running.", processName);
        return -1;
    }

    portCount = collectPorts(pids, pidCount, ports, MAX_PORTS);
    if (portCount == 0) {
        if (errBuf)
            snprintf(errBuf, errSize,
                "Process '%s' has no active network ports.", processName);
        return -1;
    }

    LOG("procfilter: %s -> %d PID(s), %d port(s)", processName, pidCount, portCount);

    // Build: " and (tcp.SrcPort == P1 or tcp.DstPort == P1 or
    //          udp.SrcPort == P1 or udp.DstPort == P1 or ...)"
    // For outbound packets local port = SrcPort; for inbound = DstPort.
    // Matching both directions ensures we catch the process's traffic.
    pos = snprintf(filterBuf, bufSize, " and (");

    for (i = 0; i < portCount; i++) {
        if (i > 0)
            pos += snprintf(filterBuf + pos, bufSize - pos, " or ");

        // safety: stop if we're running out of buffer space
        if (pos > bufSize - 120) {
            LOG("procfilter: filter truncated at %d/%d ports", i, portCount);
            break;
        }

        pos += snprintf(filterBuf + pos, bufSize - pos,
            "tcp.SrcPort == %d or tcp.DstPort == %d or "
            "udp.SrcPort == %d or udp.DstPort == %d",
            ports[i], ports[i], ports[i], ports[i]);
    }

    pos += snprintf(filterBuf + pos, bufSize - pos, ")");

    return portCount;
}
