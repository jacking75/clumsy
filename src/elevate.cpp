// clumsy needs to be run as admin to work properly.
// here's routines for checking admin state and self elevate
// from the example CppSelfElevate
// http://code.msdn.microsoft.com/windowsdesktop/CppUACSelfElevation-981c0160
#include <Windows.h>
#include <VersionHelpers.h>
#include <shellapi.h>
#include <ctype.h>
#include "common.h"

// 
//   FUNCTION: IsRunAsAdmin()
//
//   PURPOSE: The function checks whether the current process is run as 
//   administrator. In other words, it dictates whether the primary access 
//   token of the process belongs to user account that is a member of the 
//   local Administrators group and it is elevated.
//
//   RETURN VALUE: Returns TRUE if the primary access token of the process 
//   belongs to user account that is a member of the local Administrators 
//   group and it is elevated. Returns FALSE if the token does not.
//
//   EXCEPTION: If this function fails, it throws a C++ DWORD exception which 
//   contains the Win32 error code of the failure.
//   * changed to not throw and return false *
//
//   EXAMPLE CALL:
//     try 
//     {
//         if (IsRunAsAdmin())
//             wprintf (L"Process is run as administrator\n");
//         else
//             wprintf (L"Process is not run as administrator\n");
//     }
//     catch (DWORD dwError)
//     {
//         wprintf(L"IsRunAsAdmin failed w/err %lu\n", dwError);
//     }
//
BOOL IsRunAsAdmin()
{
    BOOL fIsRunAsAdmin = FALSE;
    DWORD dwError = ERROR_SUCCESS;
    PSID pAdministratorsGroup = NULL;

    // Allocate and initialize a SID of the administrators group.
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(
        &NtAuthority, 
        2, 
        SECURITY_BUILTIN_DOMAIN_RID, 
        DOMAIN_ALIAS_RID_ADMINS, 
        0, 0, 0, 0, 0, 0, 
        &pAdministratorsGroup))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

    // Determine whether the SID of administrators group is enabled in 
    // the primary access token of the process.
    if (!CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

Cleanup:
    // Centralized cleanup for all allocated resources.
    if (pAdministratorsGroup)
    {
        FreeSid(pAdministratorsGroup);
        pAdministratorsGroup = NULL;
    }

    // Throw the error if something failed in the function.
    if (ERROR_SUCCESS != dwError)
    {
        return FALSE;
    }

    return fIsRunAsAdmin;
}

// pasta from:
// http://stackoverflow.com/questions/8046097/how-to-check-if-a-process-has-the-admin-rights
BOOL IsElevated( ) {
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if( OpenProcessToken( GetCurrentProcess( ),TOKEN_QUERY,&hToken ) ) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof( TOKEN_ELEVATION );
        if( GetTokenInformation( hToken, TokenElevation, &Elevation, sizeof( Elevation ), &cbSize ) ) {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if( hToken ) {
        CloseHandle( hToken );
    }
    return fRet;
}

// Return the argument portion of a command line, i.e. everything after the
// (possibly quoted) program name.
static const char* skipProgramName(const char *cmdLine) {
    const char *p = cmdLine;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') p++;
        if (*p == '"') p++;
    } else {
        while (*p && !isspace((unsigned char)*p)) p++;
    }
    while (isspace((unsigned char)*p)) p++;
    return p;
}

// Relaunch self elevated via the UAC "runas" verb.
//
// Console builds have no window to parent a MessageBox to, so failures are
// reported on stdout. Returns TRUE when the caller should exit: either a new
// elevated instance was launched, or elevation is impossible here.
BOOL tryElevate(BOOL silent) {
    char szPath[MAX_PATH];
    SHELLEXECUTEINFOA sei = { sizeof(sei) };

    if (!IsWindowsVistaOrGreater()) {
        if (!silent) {
            INFO("Unsupported Windows version. clumsy only supports Windows Vista or above.");
        }
        return TRUE;
    }

    if (IsRunAsAdmin()) {
        return FALSE; // nothing to do, keep running
    }

    // A path that did not fit comes back as nSize, not 0, with the result
    // truncated and (pre-Windows-10) not even null-terminated. Relaunching a
    // truncated path either fails or, worse, names a different file, so treat
    // "filled the buffer exactly" as the failure it is.
    {
        const DWORD n = GetModuleFileNameA(NULL, szPath, ARRAYSIZE(szPath));
        if (n == 0 || n >= ARRAYSIZE(szPath)) {
            szPath[ARRAYSIZE(szPath) - 1] = '\0';
            if (!silent) {
                INFO("Failed to resolve the clumsy executable path%s; cannot elevate.",
                     n ? " (it is longer than " STR(MAX_PATH) " characters)" : "");
            }
            return TRUE;
        }
    }

    // Launch itself as administrator. The new process gets its own console.
    // GetCommandLineA() still has argv[0] in front, so skip past it — passing it
    // through would show up as a stray argument and fail parseArgs().
    sei.lpVerb = "runas";
    sei.lpFile = szPath;
    sei.lpParameters = skipProgramName(GetCommandLineA());
    sei.nShow = SW_NORMAL;

    LOG("Try elevating by runas");
    if (!ShellExecuteExA(&sei)) {
        DWORD dwError = GetLastError();
        if (dwError == ERROR_CANCELLED) {
            INFO("Elevation was refused. Run this console as Administrator instead.");
        } else {
            INFO("Elevation failed (code %lu).", dwError);
        }
        return TRUE;
    }

    INFO("Relaunched elevated in a new console window.");
    return TRUE;
}
