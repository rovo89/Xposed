/**
 * This file includes the Xposed service, which is especially used to work around SELinux restrictions.
 */

#define LOG_TAG "Xposed"

#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "xposed.h"
#include "xposed_service.h"
#include "xposed_logcat.h"


namespace xposed {
namespace logcat {

////////////////////////////////////////////////////////////
// Declarations
////////////////////////////////////////////////////////////

#define AID_LOG      1007
#define CAP_SYSLOG   34
char marker[50];


////////////////////////////////////////////////////////////
// Functions
////////////////////////////////////////////////////////////

static void execLogcat() {
    // Ensure that we're allowed to read all log entries
    setresgid(AID_LOG, AID_LOG, AID_LOG);
    int8_t keep[] = { CAP_SYSLOG, -1 };
    xposed::dropCapabilities(keep);

    // Execute a logcat command that will keep running in the background
    if (zygote_access(XPOSEDLOG_CONF_ALL, F_OK) == 0) {
        execl("/system/bin/logcat", "logcat",
            "-v", "time",            // include timestamps in the log
            (char*) 0);
    } else {
        execl("/system/bin/logcat", "logcat",
            "-v", "time",            // include timestamps in the log
            "-s",                    // be silent by default, except for the following tags
            "XposedStartupMarker:D", // marks the beginning of the current log
            "Xposed:I",              // Xposed framework and default logging
            "appproc:I",             // app_process
            "XposedInstaller:I",     // Xposed Installer
            "art:F",                 // ART crashes
            (char*) 0);
    }

    // We only get here in case of errors
    ALOGE("Could not execute logcat: %s", strerror(errno));
    exit(EXIT_FAILURE);
}

#ifndef dprintf
static inline int dprintf(int fd, const char *format, ...) {
    char* message;
    va_list args;
    va_start(args, format);
    int size = vasprintf(&message, format, args);
    if (size > 0) {
        write(fd, message, size);
        free(message);
    }
    va_end(args);
    return size;
}
#endif

static void runDaemon(int pipefd) {
    xposed::setProcessName("xposed_logcat");
    xposed::dropCapabilities();

    umask(0);
    int logfile = open(XPOSEDLOG, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (logfile < 0) {
        ALOGE("Could not open %s: %s", XPOSEDLOG, strerror(errno));
        exit(EXIT_FAILURE);
    }

    FILE* pipe = fdopen(pipefd, "r");
    if (pipe == NULL) {
        ALOGE("fdopen failed for pipe file descriptor %d: %s", pipefd, strerror(errno));
        exit(EXIT_FAILURE);
    }

    char buf[512];
    bool foundMarker = false;
    long totalSize = 0;
    while (fgets(buf, sizeof(buf), pipe) != NULL) {
        if (buf[0] == '-')
            continue; // beginning of <logbuffer type>

        if (!foundMarker) {
            if (strstr(buf, "XposedStartupMarker") != NULL && strstr(buf, marker) != NULL) {
                foundMarker = true;
            }
            continue;
        }

        int len = strlen(buf);
        write(logfile, buf, len);

        totalSize += len;
        if (totalSize > XPOSEDLOG_MAX_SIZE) {
            dprintf(logfile, "\nReached maximum log size (%'d kB), further lines won't be logged.\n", XPOSEDLOG_MAX_SIZE / 1024);
            exit(EXIT_FAILURE);
        }
    }

    ALOGE("Broken pipe to logcat: %s", strerror(ferror(pipe)));
    close(logfile);
    exit(EXIT_FAILURE);
}

void printStartupMarker() {
    sprintf(marker, "Current time: %d, PID: %d", (int) time(NULL), getpid());
    ALOG(LOG_DEBUG, "XposedStartupMarker", marker, NULL);
}

void start() {
    // Fork to create a daemon
    pid_t pid;
    if ((pid = fork()) < 0) {
        ALOGE("Fork for Xposed logcat daemon failed: %s", strerror(errno));
        return;
    } else if (pid != 0) {
        return;
    }

#if XPOSED_WITH_SELINUX
    if (xposed->isSELinuxEnabled) {
        if (setcon(ctx_app) != 0) {
            ALOGE("Could not switch to %s context", ctx_app);
            exit(EXIT_FAILURE);
        }
    }
#endif  // XPOSED_WITH_SELINUX

    int err = rename(XPOSEDLOG, XPOSEDLOG_OLD);
    if (err < 0 && errno != ENOENT) {
        ALOGE("%s while renaming log file %s -> %s", strerror(errno), XPOSEDLOG, XPOSEDLOG_OLD);
    }

    int pipeFds[2];
    if (pipe(pipeFds) < 0) {
        ALOGE("Could not allocate pipe for logcat output: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if ((pid = fork()) < 0) {
        ALOGE("Fork for logcat execution failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        close(pipeFds[1]);
        runDaemon(pipeFds[0]);
    } else {
        close(pipeFds[0]);
        if (dup2(pipeFds[1], STDOUT_FILENO) == -1) {
            ALOGE("Could not redirect stdout: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (dup2(pipeFds[1], STDERR_FILENO) == -1) {
            ALOGE("Could not redirect stdout: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
        execLogcat();
    }

    // Should never reach this point
    exit(EXIT_FAILURE);
}

}  // namespace logcat
}  // namespace xposed
