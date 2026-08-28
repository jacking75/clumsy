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

// One attempt to size the table, one to fetch it - and a few retries, because
// the two calls are not atomic. A connection opened in between makes the fetch
// come back ERROR_INSUFFICIENT_BUFFER as well, and treating that as failure
// silently drops every port of that protocol. --process on a game client that
// is still opening sockets is exactly the case that hits it.
#define TABLE_FETCH_ATTEMPTS 5

typedef DWORD (*TableFetchFn)(PVOID table, PDWORD size, ULONG family);

static DWORD fetchTcpTable(PVOID table, PDWORD size, ULONG family) {
    return GetExtendedTcpTable(table, size, FALSE, family,
                               TCP_TABLE_OWNER_PID_ALL, 0);
}

static DWORD fetchUdpTable(PVOID table, PDWORD size, ULONG family) {
    return GetExtendedUdpTable(table, size, FALSE, family,
                               UDP_TABLE_OWNER_PID, 0);
}

// Returns a malloc'd table the caller must free, or NULL when there is nothing
// to read (which is not an error - a host with no IPv6 sockets is normal).
static void* fetchTable(TableFetchFn fetch, ULONG family, const char *what) {
    DWORD size = 0;
    int   attempt;

    for (attempt = 0; attempt < TABLE_FETCH_ATTEMPTS; ++attempt) {
        void *table;
        DWORD rc = fetch(NULL, &size, family);

        if (rc == NO_ERROR || size == 0) return NULL;   // nothing to read
        if (rc != ERROR_INSUFFICIENT_BUFFER) {
            LOG("procfilter: could not size the %s table (%lu)", what, rc);
            return NULL;
        }

        table = malloc(size);
        if (!table) {
            LOG("procfilter: out of memory sizing the %s table", what);
            return NULL;
        }

        rc = fetch(table, &size, family);
        if (rc == NO_ERROR) return table;

        free(table);
        if (rc != ERROR_INSUFFICIENT_BUFFER) {
            LOG("procfilter: could not read the %s table (%lu)", what, rc);
            return NULL;
        }
        // The table grew between the two calls; `size` now holds what it needs.
    }

    LOG("procfilter: the %s table kept growing; giving up after %d attempts, "
        "some of its ports are missing from the filter",
        what, TABLE_FETCH_ATTEMPTS);
    return NULL;
}

// Appends one local port (network byte order, as the tables report it) unless
// it is zero or already present. Returns the new count.
static int addPort(WORD *ports, int count, int maxPorts, DWORD rawLocalPort) {
    WORD p = ntohs((WORD)rawLocalPort);
    if (p == 0 || count >= maxPorts || isPortDup(p, ports, count)) return count;
    ports[count++] = p;
    return count;
}

static int collectPorts(const DWORD *pids, int pidCount,
                        WORD *ports, int maxPorts) {
    int portCount = 0;

    // ---- TCP (IPv4) ----
    {
        PMIB_TCPTABLE_OWNER_PID t =
            (PMIB_TCPTABLE_OWNER_PID)fetchTable(fetchTcpTable, AF_INET, "TCP/IPv4");
        if (t) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount))
                    portCount = addPort(ports, portCount, maxPorts,
                                        t->table[i].dwLocalPort);
            }
            free(t);
        }
    }

    // ---- UDP (IPv4) ----
    {
        PMIB_UDPTABLE_OWNER_PID t =
            (PMIB_UDPTABLE_OWNER_PID)fetchTable(fetchUdpTable, AF_INET, "UDP/IPv4");
        if (t) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount))
                    portCount = addPort(ports, portCount, maxPorts,
                                        t->table[i].dwLocalPort);
            }
            free(t);
        }
    }

    // ---- TCP (IPv6) ----
    {
        PMIB_TCP6TABLE_OWNER_PID t =
            (PMIB_TCP6TABLE_OWNER_PID)fetchTable(fetchTcpTable, AF_INET6, "TCP/IPv6");
        if (t) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount))
                    portCount = addPort(ports, portCount, maxPorts,
                                        t->table[i].dwLocalPort);
            }
            free(t);
        }
    }

    // ---- UDP (IPv6) ----
    {
        PMIB_UDP6TABLE_OWNER_PID t =
            (PMIB_UDP6TABLE_OWNER_PID)fetchTable(fetchUdpTable, AF_INET6, "UDP/IPv6");
        if (t) {
            DWORD i;
            for (i = 0; i < t->dwNumEntries && portCount < maxPorts; i++) {
                if (isPidMatch(t->table[i].dwOwningPid, pids, pidCount))
                    portCount = addPort(ports, portCount, maxPorts,
                                        t->table[i].dwLocalPort);
            }
            free(t);
        }
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
    // appendf() clamps rather than accumulating snprintf's "would have
    // written" return value, which is what keeps the closing ")" below inside
    // the buffer no matter how the loop exits. The margin covers one whole
    // fragment (about 100 bytes with five-digit ports) plus the ")".
    pos = appendf(filterBuf, bufSize, 0, " and (");

    for (i = 0; i < portCount; i++) {
        // safety: stop if we're running out of buffer space
        if (pos > bufSize - 120) {
            LOG("procfilter: filter truncated at %d/%d ports", i, portCount);
            break;
        }
        pos = appendf(filterBuf, bufSize, pos,
            "%stcp.SrcPort == %d or tcp.DstPort == %d or "
            "udp.SrcPort == %d or udp.DstPort == %d",
            i ? " or " : "", ports[i], ports[i], ports[i], ports[i]);
    }

    appendf(filterBuf, bufSize, pos, ")");

    return portCount;
}
