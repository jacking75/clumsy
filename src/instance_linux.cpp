// Instance registry: Linux half  (multi-instance)
//
// Records live in /tmp/clumsy-instances/<pid>.txt. /tmp because clumsy runs
// both under sudo and unprivileged with file capabilities, and both kinds of
// instance must see each other's records; the directory is sticky and
// world-writable like /tmp itself.
//
// An instance holds an exclusive flock on its own record while it captures.
// The kernel drops that lock when the process dies, however it dies, so a
// record whose lock can be taken belongs to a dead process: it is skipped and
// removed. The check-then-record step is serialized by an flock on ".lock".

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

#define REGISTRY_DIR "/tmp/clumsy-instances"

namespace {

int  lockFd = -1;
int  ownFd  = -1;
char ownPath[128] = "";

int ensureDir(char *errBuf, int errSize) {
    if (mkdir(REGISTRY_DIR, 01777) == 0) {
        chmod(REGISTRY_DIR, 01777);          // mkdir's mode is cut by the umask
    } else if (errno != EEXIST) {
        snprintf(errBuf, (size_t)errSize, "cannot create the instance registry %s (%s)",
                 REGISTRY_DIR, strerror(errno));
        return 0;
    }
    return 1;
}

} // namespace

int regLock(char *errBuf, int errSize) {
    if (!ensureDir(errBuf, errSize)) return 0;
    // Read-only is enough for flock, and lets an unprivileged instance use a
    // lock file a root instance created.
    lockFd = open(REGISTRY_DIR "/.lock", O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
    if (lockFd < 0) {
        snprintf(errBuf, (size_t)errSize, "cannot open %s/.lock (%s)", REGISTRY_DIR, strerror(errno));
        return 0;
    }
    while (flock(lockFd, LOCK_EX) != 0) {
        if (errno == EINTR) continue;
        snprintf(errBuf, (size_t)errSize, "cannot lock %s/.lock (%s)", REGISTRY_DIR, strerror(errno));
        close(lockFd);
        lockFd = -1;
        return 0;
    }
    return 1;
}

void regUnlock(void) {
    if (lockFd >= 0) {
        flock(lockFd, LOCK_UN);
        close(lockFd);
        lockFd = -1;
    }
}

void regForEachLive(void (*fn)(const char *text, void *ctx), void *ctx) {
    char own[64];
    DIR *dir = opendir(REGISTRY_DIR);
    struct dirent *e;

    if (!dir) return;
    snprintf(own, sizeof(own), "%lu.txt", (unsigned long)getpid());
    while ((e = readdir(dir)) != NULL) {
        const size_t len = strlen(e->d_name);
        char path[300], text[FILTER_BUFSIZE * 6];

        if (len < 5 || strcmp(e->d_name + len - 4, ".txt") != 0) continue;
        if (strcmp(e->d_name, own) == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", REGISTRY_DIR, e->d_name);

        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        if (flock(fd, LOCK_SH | LOCK_NB) == 0) {
            // Nobody holds it: the owner is gone. In a sticky directory only
            // the owner (or root) can remove it, so this may fail; harmless.
            unlink(path);
            close(fd);
            continue;
        }
        const ssize_t got = read(fd, text, sizeof(text) - 1);
        close(fd);
        if (got > 0) {
            text[got] = '\0';
            fn(text, ctx);
        }
    }
    closedir(dir);
}

int regWriteOwn(const char *text, char *errBuf, int errSize) {
    const size_t len = strlen(text);

    regRemoveOwn();
    snprintf(ownPath, sizeof(ownPath), "%s/%lu.txt", REGISTRY_DIR, (unsigned long)getpid());
    ownFd = open(ownPath, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (ownFd < 0 || flock(ownFd, LOCK_EX | LOCK_NB) != 0 ||
        write(ownFd, text, len) != (ssize_t)len) {
        snprintf(errBuf, (size_t)errSize, "cannot write the instance record %s (%s)",
                 ownPath, strerror(errno));
        regRemoveOwn();
        return 0;
    }
    fchmod(ownFd, 0644);                     // readable whatever the umask
    return 1;
}

void regRemoveOwn(void) {
    if (ownFd >= 0) {
        unlink(ownPath);
        close(ownFd);
        ownFd = -1;
    }
    ownPath[0] = '\0';
}
