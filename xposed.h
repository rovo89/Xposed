#ifndef XPOSED_H_
#define XPOSED_H_

#define ANDROID_SMP 0
#include "Dalvik.h"


namespace android {

#define XPOSED_DIR "/data/data/de.robv.android.xposed.installer/"
#define XPOSED_JAR XPOSED_DIR "bin/XposedBridge.jar"
#define XPOSED_JAR_NEWVERSION XPOSED_DIR "bin/XposedBridge.jar.newversion"
#define XPOSED_LOAD_BLOCKER XPOSED_DIR "conf/disabled"
#define XPOSED_SAFEMODE_NODELAY XPOSED_DIR "conf/safemode_nodelay"
#define XPOSED_CLASS "de/robv/android/xposed/XposedBridge"
#define XPOSED_CLASS_DOTS "de.robv.android.xposed.XposedBridge"
#define XRESOURCES_CLASS "android/content/res/XResources"
#define MIUI_RESOURCES_CLASS "android/content/res/MiuiResources"
#define XPOSED_VERSION "47"

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
bool xposedShouldIgnoreCommand(const char* className, int argc, const char* const argv[]);
bool addXposedToClasspath(bool zygote);
bool xposedOnVmCreated(JNIEnv* env, const char* className);
static bool xposedInitMemberOffsets(JNIEnv* env);
static inline void xposedSetObjectArrayElement(const ArrayObject* obj, int index, Object* val);

// handling hooked methods / helpers
static void xposedCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
static jobject xposedAddLocalReference(::Thread* self, Object* obj);
static void replaceAsm(uintptr_t function, unsigned const char* newCode, size_t len);
static void patchReturnTrue(uintptr_t function);
static inline bool xposedIsHooked(const Method* method);

// JNI methods
static jboolean de_robv_android_xposed_XposedBridge_initNative(JNIEnv* env, jclass clazz);
static void de_robv_android_xposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethodIndirect,
            jobject declaredClassIndirect, jint slot, jobject additionalInfoIndirect);
static void de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
static void android_content_res_XResources_rewriteXmlReferencesNative(JNIEnv* env, jclass clazz,
            jint parserPtr, jobject origRes, jobject repRes);
static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env);
static int register_android_content_res_XResources(JNIEnv* env);
}

#endif  // XPOSED_H_
