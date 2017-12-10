#ifndef XPOSED_H_
#define XPOSED_H_

#include "xposed_shared.h"

#define XPOSED_PROP_FILE "/system/xposed.prop"

#if defined(__LP64__)
  #define XPOSED_LIB_DIR "/system/lib64/"
#else
  #define XPOSED_LIB_DIR "/system/lib/"
#endif
#define XPOSED_LIB_DALVIK        XPOSED_LIB_DIR "libxposed_dalvik.so"
#define XPOSED_LIB_ART           XPOSED_LIB_DIR "libxposed_art.so"
#define XPOSED_JAR               "/system/framework/XposedBridge.jar"
#define XPOSED_JAR_NEWVERSION    XPOSED_DIR "bin/XposedBridge.jar.newversion"
#define XPOSED_LOAD_BLOCKER      XPOSED_DIR "conf/disabled"
#define XPOSED_SAFEMODE_NODELAY  XPOSED_DIR "conf/safemode_nodelay"
#define XPOSED_SAFEMODE_DISABLE  XPOSED_DIR "conf/safemode_disable"

#define XPOSED_CLASS_DOTS_ZYGOTE "de.robv.android.xposed.XposedBridge"
#define XPOSED_CLASS_DOTS_TOOLS  "de.robv.android.xposed.XposedBridge$ToolEntryPoint"

#if XPOSED_WITH_SELINUX
#include <selinux/selinux.h>
#define ctx_system ((security_context_t) "u:r:system_server:s0")
#if PLATFORM_SDK_VERSION >= 23
#define ctx_app    ((security_context_t) "u:r:untrusted_app:s0:c512,c768")
#else
#define ctx_app    ((security_context_t) "u:r:untrusted_app:s0")
#endif  // PLATFORM_SDK_VERSION >= 23
#endif  // XPOSED_WITH_SELINUX

namespace xposed {

    bool handleOptions(int argc, char* const argv[]);
    bool initialize(bool zygote, bool startSystemServer, const char* className, int argc, char* const argv[]);
    void printRomInfo();
    void parseXposedProp();
    int getSdkVersion();
    bool isDisabled();
    void disableXposed();
    bool isSafemodeDisabled();
    bool shouldSkipSafemodeDelay();
    bool shouldIgnoreCommand(int argc, const char* const argv[]);
    bool addJarToClasspath();
    void onVmCreated(JNIEnv* env);
    void setProcessName(const char* name);
    bool determineXposedInstallerUidGid();
    bool switchToXposedInstallerUidGid();
    void dropCapabilities(int8_t keep[] = NULL);
    bool isMinimalFramework();

}  // namespace xposed

#endif  // XPOSED_H_
