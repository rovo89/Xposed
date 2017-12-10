/**
 * These declarations are needed for both app_process and the libraries.
 */

#ifndef XPOSED_SHARED_H_
#define XPOSED_SHARED_H_

#include <sys/stat.h>

#include "cutils/log.h"
#include "jni.h"

#ifndef ALOG
#define ALOG  LOG
#define ALOGD LOGD
#define ALOGD LOGD
#define ALOGE LOGE
#define ALOGI LOGI
#define ALOGV LOGV
#endif

#if PLATFORM_SDK_VERSION >= 24
#define XPOSED_DIR "/data/user_de/0/de.robv.android.xposed.installer/"
#else
#define XPOSED_DIR "/data/data/de.robv.android.xposed.installer/"
#endif

namespace xposed {

struct XposedShared {
    // Global variables
    bool zygote;
    bool startSystemServer;
    const char* startClassName;
    uint32_t xposedVersionInt;
    bool isSELinuxEnabled;
    bool isSELinuxEnforcing;
    uid_t installer_uid;
    gid_t installer_gid;

    // Provided by runtime-specific library, used by executable
    void (*onVmCreated)(JNIEnv* env);

#if XPOSED_WITH_SELINUX
    // Provided by the executable, used by runtime-specific library
    int (*zygoteservice_accessFile)(const char* path, int mode);
    int (*zygoteservice_statFile)(const char* path, struct stat* st);
    char* (*zygoteservice_readFile)(const char* path, int* bytesRead);
#endif
};

extern XposedShared* xposed;

} // namespace xposed

#endif // XPOSED_SHARED_H_
