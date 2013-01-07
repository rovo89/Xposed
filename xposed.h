#ifndef XPOSED_H_
#define XPOSED_H_

#define ANDROID_SMP 0
#include "Dalvik.h"
#include <list>

namespace android {

#define XPOSED_JAR "/data/xposed/XposedBridge.jar"
#define XPOSED_JAR_NEWVERSION XPOSED_JAR ".newversion"
#define XPOSED_LOAD_BLOCKER "/data/xposed/disabled"
#define XPOSED_CLASS "de/robv/android/xposed/XposedBridge"
#define XPOSED_CLASS_DOTS "de.robv.android.xposed.XposedBridge"
#define XRESOURCES_CLASS "android/content/res/XResources"
#define XPOSED_VERSION "28"

#ifndef ALOGD
#define ALOGD LOGD
#define ALOGE LOGE
#define ALOGI LOGI
#define ALOGV LOGV
#endif

extern bool keepLoadingXposed;
typedef std::list<Method>::iterator XposedOriginalMethodsIt;

// called directoy by app_process
bool isXposedDisabled();
bool xposedShouldIgnoreCommand(const char* className, int argc, const char* const argv[]);
bool addXposedToClasspath(bool zygote);
bool xposedOnVmCreated(JNIEnv* env, const char* className);

// handling hooked methods / helpers
static void xposedCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
static XposedOriginalMethodsIt findXposedOriginalMethod(const Method* method);
static jobject xposedAddLocalReference(::Thread* self, Object* obj);
static void replaceAsm(void* function, char* newCode, int len);

// JNI methods
static void de_robv_android_xposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod);
static jobject de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod,
            jobjectArray params1, jclass returnType1, jobject thisObject1, jobjectArray args1);
static void android_content_res_XResources_rewriteXmlReferencesNative(JNIEnv* env, jclass clazz,
            jint parserPtr, jobject origRes, jobject repRes);
static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env);
static int register_android_content_res_XResources(JNIEnv* env);
}

#endif  // XPOSED_H_
