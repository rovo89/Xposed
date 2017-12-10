/**
 * This file includes functions called directly from app_main.cpp during startup.
 */

#define LOG_TAG "Xposed"

#include "xposed.h"
#include "xposed_logcat.h"
#include "xposed_safemode.h"
#include "xposed_service.h"

#include <cstring>
#include <ctype.h>
#include <cutils/process_name.h>
#include <cutils/properties.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>

#if PLATFORM_SDK_VERSION >= 18
#include <sys/capability.h>
#else
#include <linux/capability.h>
#endif

namespace xposed {

////////////////////////////////////////////////////////////
// Variables
////////////////////////////////////////////////////////////

XposedShared* xposed = new XposedShared;
static int sdkVersion = -1;
static char* argBlockStart;
static size_t argBlockLength;

const char* xposedVersion = "unknown (invalid " XPOSED_PROP_FILE ")";
uint32_t xposedVersionInt = 0;

////////////////////////////////////////////////////////////
// Functions
////////////////////////////////////////////////////////////

/** Handle special command line options. */
bool handleOptions(int argc, char* const argv[]) {
    parseXposedProp();

    if (argc == 2 && strcmp(argv[1], "--xposedversion") == 0) {
        printf("Xposed version: %s\n", xposedVersion);
        return true;
    }

    if (argc == 2 && strcmp(argv[1], "--xposedtestsafemode") == 0) {
        printf("Testing Xposed safemode trigger\n");

        if (detectSafemodeTrigger(shouldSkipSafemodeDelay())) {
            printf("Safemode triggered\n");
        } else {
            printf("Safemode not triggered\n");
        }
        return true;
    }

    // From Lollipop coding, used to override the process name
    argBlockStart = argv[0];
    uintptr_t start = reinterpret_cast<uintptr_t>(argv[0]);
    uintptr_t end = reinterpret_cast<uintptr_t>(argv[argc - 1]);
    end += strlen(argv[argc - 1]) + 1;
    argBlockLength = end - start;

    return false;
}

/** Initialize Xposed (unless it is disabled). */
bool initialize(bool zygote, bool startSystemServer, const char* className, int argc, char* const argv[]) {
#if !defined(XPOSED_ENABLE_FOR_TOOLS)
    if (!zygote)
        return false;
#endif

    if (isMinimalFramework()) {
        ALOGI("Not loading Xposed for minimal framework (encrypted device)");
        return false;
    }

    xposed->zygote = zygote;
    xposed->startSystemServer = startSystemServer;
    xposed->startClassName = className;
    xposed->xposedVersionInt = xposedVersionInt;

#if XPOSED_WITH_SELINUX
    xposed->isSELinuxEnabled   = is_selinux_enabled() == 1;
    xposed->isSELinuxEnforcing = xposed->isSELinuxEnabled && security_getenforce() == 1;
#else
    xposed->isSELinuxEnabled   = false;
    xposed->isSELinuxEnforcing = false;
#endif  // XPOSED_WITH_SELINUX

    if (startSystemServer) {
        xposed::logcat::printStartupMarker();
    } else if (zygote) {
        // TODO Find a better solution for this
        // Give the primary Zygote process a little time to start first.
        // This also makes the log easier to read, as logs for the two Zygotes are not mixed up.
        sleep(10);
    }

    printRomInfo();

    if (startSystemServer) {
        if (!determineXposedInstallerUidGid() || !xposed::service::startAll()) {
            return false;
        }
        xposed::logcat::start();
#if XPOSED_WITH_SELINUX
    } else if (xposed->isSELinuxEnabled) {
        if (!xposed::service::startMembased()) {
            return false;
        }
#endif  // XPOSED_WITH_SELINUX
    }

#if XPOSED_WITH_SELINUX
    // Don't let any further forks access the Zygote service
    if (xposed->isSELinuxEnabled) {
        xposed::service::membased::restrictMemoryInheritance();
    }
#endif  // XPOSED_WITH_SELINUX

    // FIXME Zygote has no access to input devices, this would need to be check in system_server context
    if (zygote && !isSafemodeDisabled() && detectSafemodeTrigger(shouldSkipSafemodeDelay()))
        disableXposed();

    if (isDisabled() || (!zygote && shouldIgnoreCommand(argc, argv)))
        return false;

    return addJarToClasspath();
}

/** Print information about the used ROM into the log */
void printRomInfo() {
    char release[PROPERTY_VALUE_MAX];
    char sdk[PROPERTY_VALUE_MAX];
    char manufacturer[PROPERTY_VALUE_MAX];
    char model[PROPERTY_VALUE_MAX];
    char rom[PROPERTY_VALUE_MAX];
    char fingerprint[PROPERTY_VALUE_MAX];
    char platform[PROPERTY_VALUE_MAX];
#if defined(__LP64__)
    const int bit = 64;
#else
    const int bit = 32;
#endif

    property_get("ro.build.version.release", release, "n/a");
    property_get("ro.build.version.sdk", sdk, "n/a");
    property_get("ro.product.manufacturer", manufacturer, "n/a");
    property_get("ro.product.model", model, "n/a");
    property_get("ro.build.display.id", rom, "n/a");
    property_get("ro.build.fingerprint", fingerprint, "n/a");
    property_get("ro.product.cpu.abi", platform, "n/a");

    ALOGI("-----------------");
    ALOGI("Starting Xposed version %s, compiled for SDK %d", xposedVersion, PLATFORM_SDK_VERSION);
    ALOGI("Device: %s (%s), Android version %s (SDK %s)", model, manufacturer, release, sdk);
    ALOGI("ROM: %s", rom);
    ALOGI("Build fingerprint: %s", fingerprint);
    ALOGI("Platform: %s, %d-bit binary, system server: %s", platform, bit, xposed->startSystemServer ? "yes" : "no");
    if (!xposed->zygote) {
        ALOGI("Class name: %s", xposed->startClassName);
    }
    ALOGI("SELinux enabled: %s, enforcing: %s",
            xposed->isSELinuxEnabled ? "yes" : "no",
            xposed->isSELinuxEnforcing ? "yes" : "no");
}

/** Parses /system/xposed.prop and stores selected values in variables */
void parseXposedProp() {
    FILE *fp = fopen(XPOSED_PROP_FILE, "r");
    if (fp == NULL) {
        ALOGE("Could not read %s: %s", XPOSED_PROP_FILE, strerror(errno));
        return;
    }

    char buf[512];
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        char* key = buf;
        // Ignore leading spaces for the key
        while (isspace(*key)) key++;

        // Skip comments
        if (*key == '#')
            continue;

        // Find the key/value separator
        char* value = strchr(buf, '=');
        if (value == NULL)
            continue;

        // Ignore trailing spaces for the key
        char* tmp = value;
        do { *tmp = 0; tmp--; } while (isspace(*tmp));

        // Ignore leading spaces for the value
        do { value++; } while (isspace(*value));

        // Remove trailing newline
        tmp = strpbrk(value, "\n\r");
        if (tmp != NULL)
            *tmp = 0;

        // Handle this entry
        if (!strcmp("version", key)) {
            int len = strlen(value);
            if (len == 0)
                continue;
            tmp = (char*) malloc(len + 1);
            strlcpy(tmp, value, len + 1);
            xposedVersion = tmp;
            xposedVersionInt = atoi(xposedVersion);
        }
    }
    fclose(fp);

    return;
}

/** Returns the SDK version of the system */
int getSdkVersion() {
    if (sdkVersion < 0) {
        char sdkString[PROPERTY_VALUE_MAX];
        property_get("ro.build.version.sdk", sdkString, "0");
        sdkVersion = atoi(sdkString);
    }
    return sdkVersion;
}

/** Check whether Xposed is disabled by a flag file */
bool isDisabled() {
    if (zygote_access(XPOSED_LOAD_BLOCKER, F_OK) == 0) {
        ALOGE("Found %s, not loading Xposed", XPOSED_LOAD_BLOCKER);
        return true;
    }
    return false;
}

/** Create a flag file to disable Xposed. */
void disableXposed() {
    int fd;
    // FIXME add a "touch" operation to xposed::service::membased
    fd = open(XPOSED_LOAD_BLOCKER, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd >= 0)
        close(fd);
}

/** Check whether safemode is disabled. */
bool isSafemodeDisabled() {
    if (zygote_access(XPOSED_SAFEMODE_DISABLE, F_OK) == 0)
        return true;
    else
        return false;
}

/** Check whether the delay for safemode should be skipped. */
bool shouldSkipSafemodeDelay() {
    if (zygote_access(XPOSED_SAFEMODE_NODELAY, F_OK) == 0)
        return true;
    else
        return false;
}

/** Ignore the broadcasts by various Superuser implementations to avoid spamming the Xposed log. */
bool shouldIgnoreCommand(int argc, const char* const argv[]) {
    if (argc < 4 || strcmp(xposed->startClassName, "com.android.commands.am.Am") != 0)
        return false;

    if (strcmp(argv[2], "broadcast") != 0 && strcmp(argv[2], "start") != 0)
        return false;

    bool mightBeSuperuser = false;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "com.noshufou.android.su.RESULT") == 0
         || strcmp(argv[i], "eu.chainfire.supersu.NativeAccess") == 0)
            return true;

        if (mightBeSuperuser && strcmp(argv[i], "--user") == 0)
            return true;

        const char* lastComponent = strrchr(argv[i], '.');
        if (!lastComponent)
            continue;

        if (strcmp(lastComponent, ".RequestActivity") == 0
         || strcmp(lastComponent, ".NotifyActivity") == 0
         || strcmp(lastComponent, ".SuReceiver") == 0)
            mightBeSuperuser = true;
    }

    return false;
}

/** Adds a path to the beginning of an environment variable. */
static bool addPathToEnv(const char* name, const char* path) {
    char* oldPath = getenv(name);
    if (oldPath == NULL) {
        setenv(name, path, 1);
    } else {
        char newPath[4096];
        int neededLength = snprintf(newPath, sizeof(newPath), "%s:%s", path, oldPath);
        if (neededLength >= (int)sizeof(newPath)) {
            ALOGE("ERROR: %s would exceed %" PRIuPTR " characters", name, sizeof(newPath));
            return false;
        }
        setenv(name, newPath, 1);
    }
    return true;
}

/** Add XposedBridge.jar to the Java classpath. */
bool addJarToClasspath() {
    ALOGI("-----------------");

    // Do we have a new version and are (re)starting zygote? Then load it!
    /*
    FIXME if you can
    if (xposed->startSystemServer && access(XPOSED_JAR_NEWVERSION, R_OK) == 0) {
        ALOGI("Found new Xposed jar version, activating it");
        if (rename(XPOSED_JAR_NEWVERSION, XPOSED_JAR) != 0) {
            ALOGE("Could not move %s to %s", XPOSED_JAR_NEWVERSION, XPOSED_JAR);
            return false;
        }
    }
    */

    if (access(XPOSED_JAR, R_OK) == 0) {
        if (!addPathToEnv("CLASSPATH", XPOSED_JAR))
            return false;

        ALOGI("Added Xposed (%s) to CLASSPATH", XPOSED_JAR);
        return true;
    } else {
        ALOGE("ERROR: Could not access Xposed jar '%s'", XPOSED_JAR);
        return false;
    }
}

/** Callback which checks the loaded shared libraries for libdvm/libart. */
static bool determineRuntime(const char** xposedLibPath) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp == NULL) {
        ALOGE("Could not open /proc/self/maps: %s", strerror(errno));
        return false;
    }

    bool success = false;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char* libname = strrchr(line, '/');
        if (!libname)
            continue;
        libname++;

        if (strcmp("libdvm.so\n", libname) == 0) {
            ALOGI("Detected Dalvik runtime");
            *xposedLibPath = XPOSED_LIB_DALVIK;
            success = true;
            break;

        } else if (strcmp("libart.so\n", libname) == 0) {
            ALOGI("Detected ART runtime");
            *xposedLibPath = XPOSED_LIB_ART;
            success = true;
            break;
        }
    }

    fclose(fp);
    return success;
}

/** Load the libxposed_*.so library for the currently active runtime. */
void onVmCreated(JNIEnv* env) {
    // Determine the currently active runtime
    const char* xposedLibPath = NULL;
    if (!determineRuntime(&xposedLibPath)) {
        ALOGE("Could not determine runtime, not loading Xposed");
        return;
    }

    // Load the suitable libxposed_*.so for it
    void* xposedLibHandle = dlopen(xposedLibPath, RTLD_NOW);
    if (!xposedLibHandle) {
        ALOGE("Could not load libxposed: %s", dlerror());
        return;
    }

    // Clear previous errors
    dlerror();

    // Initialize the library
    bool (*xposedInitLib)(XposedShared* shared) = NULL;
    *(void **) (&xposedInitLib) = dlsym(xposedLibHandle, "xposedInitLib");
    if (!xposedInitLib)  {
        ALOGE("Could not find function xposedInitLib");
        return;
    }

#if XPOSED_WITH_SELINUX
    xposed->zygoteservice_accessFile = &service::membased::accessFile;
    xposed->zygoteservice_statFile   = &service::membased::statFile;
    xposed->zygoteservice_readFile   = &service::membased::readFile;
#endif  // XPOSED_WITH_SELINUX

    if (xposedInitLib(xposed)) {
        xposed->onVmCreated(env);
    }
}

/** Set the process name */
void setProcessName(const char* name) {
    memset(argBlockStart, 0, argBlockLength);
    strlcpy(argBlockStart, name, argBlockLength);
    set_process_name(name);
}

/** Determine the UID/GID of Xposed Installer. */
bool determineXposedInstallerUidGid() {
    if (xposed->isSELinuxEnabled) {
        struct stat* st = (struct stat*) mmap(NULL, sizeof(struct stat), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (st == MAP_FAILED) {
            ALOGE("Could not allocate memory in determineXposedInstallerUidGid(): %s", strerror(errno));
            return false;
        }

        pid_t pid;
        if ((pid = fork()) < 0) {
            ALOGE("Fork in determineXposedInstallerUidGid() failed: %s", strerror(errno));
            munmap(st, sizeof(struct stat));
            return false;
        } else if (pid == 0) {
            // Child.
#if XPOSED_WITH_SELINUX
            if (setcon(ctx_app) != 0) {
                ALOGE("Could not switch to %s context", ctx_app);
                exit(EXIT_FAILURE);
            }
#endif  // XPOSED_WITH_SELINUX

            if (TEMP_FAILURE_RETRY(stat(XPOSED_DIR, st)) != 0) {
                ALOGE("Could not stat %s: %s", XPOSED_DIR, strerror(errno));
                exit(EXIT_FAILURE);
            }

            exit(EXIT_SUCCESS);
        }

        // Parent.
        int status;
        if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            munmap(st, sizeof(struct stat));
            return false;
        }

        xposed->installer_uid = st->st_uid;
        xposed->installer_gid = st->st_gid;
        munmap(st, sizeof(struct stat));
        return true;
    } else {
        struct stat st;
        if (TEMP_FAILURE_RETRY(stat(XPOSED_DIR, &st)) != 0) {
            ALOGE("Could not stat %s: %s", XPOSED_DIR, strerror(errno));
            return false;
        }

        xposed->installer_uid = st.st_uid;
        xposed->installer_gid = st.st_gid;
        return true;
    }
}

/** Switch UID/GID to the ones of Xposed Installer. */
bool switchToXposedInstallerUidGid() {
    if (setresgid(xposed->installer_gid, xposed->installer_gid, xposed->installer_gid) != 0) {
        ALOGE("Could not setgid(%d): %s", xposed->installer_gid, strerror(errno));
        return false;
    }
    if (setresuid(xposed->installer_uid, xposed->installer_uid, xposed->installer_uid) != 0) {
        ALOGE("Could not setuid(%d): %s", xposed->installer_uid, strerror(errno));
        return false;
    }
    return true;
}

/** Drop all capabilities except for the mentioned ones */
void dropCapabilities(int8_t keep[]) {
    struct __user_cap_header_struct header;
    struct __user_cap_data_struct cap[2];
    memset(&header, 0, sizeof(header));
    memset(&cap, 0, sizeof(cap));
    header.version = _LINUX_CAPABILITY_VERSION_3;
    header.pid = 0;

    if (keep != NULL) {
      for (int i = 0; keep[i] >= 0; i++) {
        cap[CAP_TO_INDEX(keep[i])].permitted |= CAP_TO_MASK(keep[i]);
      }
      cap[0].effective = cap[0].inheritable = cap[0].permitted;
      cap[1].effective = cap[1].inheritable = cap[1].permitted;
    }

    capset(&header, &cap[0]);
}

/**
 * Checks whether the system is booting into a minimal Android framework.
 * This is the case when the device is encrypted with a password that
 * has to be entered on boot. /data is a tmpfs in that case, so we
 * can't load any modules anyway.
 * The system will reboot later with the full framework.
 */
bool isMinimalFramework() {
    char voldDecrypt[PROPERTY_VALUE_MAX];
    property_get("vold.decrypt", voldDecrypt, "");
    return ((strcmp(voldDecrypt, "trigger_restart_min_framework") == 0) ||
            (strcmp(voldDecrypt, "1") == 0));
}

}  // namespace xposed
