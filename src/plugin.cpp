// Plugin modules  (Phase 3.6 — optional, disabled by default)
//
// Loads extra Module implementations from a shared library so an experiment does
// not need a rebuild. A plugin exports one function:
//
//     Windows:  extern "C" __declspec(dllexport) Module* clumsyGetModule(void);
//     Linux:    extern "C" Module* clumsyGetModule(void);
//
// returning a pointer to a statically allocated Module with the same contract
// as a built-in one (see docs/CODING_STYLE.md section 2).
//
// SECURITY: loading a library runs its code with clumsy's privileges, and clumsy
// runs elevated. This is therefore off unless --enable-plugins <dir> is passed
// explicitly, and the console prints a warning naming every file it loads.
//
// Scope limit: plugins register into the same fixed-size modules[] table, whose
// size MODULE_CNT is a compile-time constant used by statslog/report/controlapi.
// Rather than make that dynamic across the whole codebase, a plugin replaces a
// built-in slot only if it declares the same shortName; otherwise it is
// reported and skipped. Growing the table is Phase 4 work.

#include <stdio.h>
#include <string.h>

#include "common.h"

#if defined(_WIN32)
#  include <Windows.h>
typedef HMODULE PluginHandle;
#  define PLUGIN_EXT ".dll"
#else
#  include <dirent.h>
#  include <dlfcn.h>
typedef void* PluginHandle;
#  define PLUGIN_EXT ".so"
#endif

#define MAX_PLUGINS 8

typedef Module* (*ClumsyGetModuleFn)(void);

static PluginHandle loaded[MAX_PLUGINS];
static int          loadedCount = 0;

// --- loader shims ---------------------------------------------------------
static PluginHandle pluginOpen(const char *path) {
#if defined(_WIN32)
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static ClumsyGetModuleFn pluginSymbol(PluginHandle h) {
#if defined(_WIN32)
    return (ClumsyGetModuleFn)GetProcAddress(h, "clumsyGetModule");
#else
    return (ClumsyGetModuleFn)dlsym(h, "clumsyGetModule");
#endif
}

static void pluginClose(PluginHandle h) {
#if defined(_WIN32)
    FreeLibrary(h);
#else
    dlclose(h);
#endif
}

static const char* pluginError(void) {
#if defined(_WIN32)
    static char buf[32];
    snprintf(buf, sizeof(buf), "error %lu", GetLastError());
    return buf;
#else
    const char *e = dlerror();
    return e ? e : "unknown error";
#endif
}

static int registerPluginModule(Module *m, const char *dllName) {
    int ix;
    if (!m || !m->shortName || !m->process || !m->enabledFlag) {
        INFO("plugin: %s returned an incomplete Module, skipping.", dllName);
        return 0;
    }
    for (ix = 0; ix < MODULE_CNT; ++ix) {
        if (strcmp(modules[ix]->shortName, m->shortName) == 0) {
            INFO("plugin: %s replaces the built-in '%s' module.", dllName, m->shortName);
            m->lastEnabled = 0;
            m->processTriggered = 0;
            m->affectedCount = 0;
            modules[ix] = m;
            return 1;
        }
    }
    INFO("plugin: %s declares module '%s', which does not match any built-in "
         "slot - skipped. Name it after the module you intend to replace.",
         dllName, m->shortName);
    return 0;
}

// Loads one candidate file. Returns 1 when a module was registered.
static int tryLoadPlugin(const char *dir, const char *fileName) {
    char full[MSG_BUFSIZE];
    PluginHandle mod;
    ClumsyGetModuleFn getModule;

    if (loadedCount >= MAX_PLUGINS) {
        INFO("plugin: reached the %d plugin limit, ignoring the rest.", MAX_PLUGINS);
        return -1;   // stop scanning
    }

#if defined(_WIN32)
    snprintf(full, sizeof(full), "%s\\%s", dir, fileName);
#else
    snprintf(full, sizeof(full), "%s/%s", dir, fileName);
#endif

    mod = pluginOpen(full);
    if (!mod) {
        INFO("plugin: cannot load %s (%s)", full, pluginError());
        return 0;
    }

    getModule = pluginSymbol(mod);
    if (!getModule) {
        INFO("plugin: %s has no clumsyGetModule export, ignoring.", fileName);
        pluginClose(mod);
        return 0;
    }

    INFO("plugin: loading %s", full);
    if (registerPluginModule(getModule(), fileName)) {
        loaded[loadedCount++] = mod;
        return 1;
    }
    pluginClose(mod);
    return 0;
}

static void printPluginWarning(void) {
    INFO("");
    INFO("  *** PLUGIN LOADING ENABLED ***");
    INFO("  clumsy runs with elevated privileges; a plugin runs with the same rights.");
    INFO("  Only load libraries you built or trust.");
}

int pluginLoadDir(const char *dir) {
    int registered = 0;

    if (!dir || !dir[0]) return 0;

#if defined(_WIN32)
    {
        WIN32_FIND_DATAA fd;
        char pattern[MSG_BUFSIZE];
        HANDLE h;

        snprintf(pattern, sizeof(pattern), "%s\\*" PLUGIN_EXT, dir);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) {
            INFO("plugin: no " PLUGIN_EXT " files found in '%s'", dir);
            return 0;
        }
        printPluginWarning();
        do {
            const int rc = tryLoadPlugin(dir, fd.cFileName);
            if (rc < 0) break;
            registered += rc;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *entry;
        int warned = 0;

        if (!d) {
            INFO("plugin: cannot open directory '%s'", dir);
            return 0;
        }
        while ((entry = readdir(d)) != NULL) {
            const size_t nameLen = strlen(entry->d_name);
            const size_t extLen  = strlen(PLUGIN_EXT);
            if (nameLen <= extLen ||
                strcmp(entry->d_name + nameLen - extLen, PLUGIN_EXT) != 0) {
                continue;
            }
            if (!warned) { printPluginWarning(); warned = 1; }
            const int rc = tryLoadPlugin(dir, entry->d_name);
            if (rc < 0) break;
            registered += rc;
        }
        closedir(d);
        if (!warned) {
            INFO("plugin: no " PLUGIN_EXT " files found in '%s'", dir);
            return 0;
        }
    }
#endif

    INFO("plugin: %d module(s) registered from '%s'", registered, dir);
    INFO("");
    return registered;
}

void pluginUnloadAll(void) {
    // Deliberately does not unload: modules[] may still point at a Module living
    // in a plugin's image, and clumsy is on its way out anyway. Unloading here
    // would only create a window for a use-after-free.
    if (loadedCount) {
        LOG("plugin: leaving %d plugin image(s) mapped until process exit", loadedCount);
    }
    loadedCount = 0;
}
