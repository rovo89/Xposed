#ifndef XPOSED_H_
#define XPOSED_H_

#define ANDROID_SMP 0
#include "Dalvik.h"


namespace android {

#define XPOSED_DIR "/data/data/de.robv.android.xposed.installer/"
#define XPOSED_JAR XPOSED_DIR "bin/XposedBridge.jar"
#define XPOSED_JAR_NEWVERSION XPOSED_DIR "bin/XposedBridge.jar.newversion"
#define XPOSED_LOAD_BLOCKER XPOSED_DIR "conf/disabled"
#define XPOSED_ENABLE_FOR_TOOLS XPOSED_DIR "conf/enable_for_tools"
#define XPOSED_SAFEMODE_NODELAY XPOSED_DIR "conf/safemode_nodelay"
#define XPOSED_SAFEMODE_DISABLE XPOSED_DIR "conf/safemode_disable"
#define XPOSED_OVERRIDE_JIT_RESET_OFFSET XPOSED_DIR "conf/jit_reset_offset"

#define XPOSED_CLASS "de/robv/android/xposed/XposedBridge"
#define XPOSED_CLASS_DOTS "de.robv.android.xposed.XposedBridge"
#define XRESOURCES_CLASS "android/content/res/XResources"
#define MIUI_RESOURCES_CLASS "android/content/res/MiuiResources"
#define XTYPEDARRAY_CLASS "android/content/res/XResources$XTypedArray"

#define XPOSED_VERSION "58"

#ifndef ALOGD
#define ALOGD LOGD
#define ALOGE LOGE
#define ALOGI LOGI
#define ALOGV LOGV
#endif

extern bool keepLoadingXposed;

struct XposedHookInfo {
    struct {
        Method originalMethod;
        // copy a few bytes more than defined for Method in AOSP
        // to accomodate for (rare) extensions by the target ROM
        int dummyForRomExtensions[4];
    } originalMethodStruct;

    Object* reflectedMethod;
    Object* additionalInfo;
};

// called directoy by app_process
void xposedInfo();
void xposedEnforceDalvik();
void disableXposed();
bool isXposedDisabled();
bool xposedSkipSafemodeDelay();
bool xposedDisableSafemode();
static int xposedReadIntConfig(const char* fileName, int defaultValue);
bool xposedShouldIgnoreCommand(const char* className, int argc, const char* const argv[]);
bool addXposedToClasspath(bool zygote);
static void xposedPrepareSubclassReplacement(jclass clazz);
bool xposedOnVmCreated(JNIEnv* env, const char* className);
static bool xposedInitMemberOffsets(JNIEnv* env);
static inline void xposedSetObjectArrayElement(const ArrayObject* obj, int index, Object* val);

// handling hooked methods / helpers
static void xposedCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
static inline bool xposedIsHooked(const Method* method);

// JNI methods
static jboolean de_robv_android_xposed_XposedBridge_initNative(JNIEnv* env, jclass clazz);
static void de_robv_android_xposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethodIndirect,
            jobject declaredClassIndirect, jint slot, jobject additionalInfoIndirect);
static void de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
static void android_content_res_XResources_rewriteXmlReferencesNative(JNIEnv* env, jclass clazz, jint parserPtr, jobject origRes, jobject repRes);
static jobject de_robv_android_xposed_XposedBridge_getStartClassName(JNIEnv* env, jclass clazz);
static void de_robv_android_xposed_XposedBridge_setObjectClassNative(JNIEnv* env, jclass clazz, jobject objIndirect, jclass clzIndirect);
static void de_robv_android_xposed_XposedBridge_dumpObjectNative(JNIEnv* env, jclass clazz, jobject objIndirect);
static jobject de_robv_android_xposed_XposedBridge_cloneToSubclassNative(JNIEnv* env, jclass clazz, jobject objIndirect, jclass clzIndirect);

static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env);
static int register_android_content_res_XResources(JNIEnv* env);

} // namespace android

#endif  // XPOSED_H_
