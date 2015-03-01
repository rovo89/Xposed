#ifndef LIBXPOSED_COMMON_H_
#define LIBXPOSED_COMMON_H_

#include "xposed_shared.h"

#ifndef NATIVE_METHOD
#define NATIVE_METHOD(className, functionName, signature) \
  { #functionName, signature, reinterpret_cast<void*>(className ## _ ## functionName) }
#endif
#define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))

namespace xposed {

#define CLASS_XPOSED_BRIDGE  "de/robv/android/xposed/XposedBridge"
#define CLASS_XRESOURCES     "android/content/res/XResources"
#define CLASS_MIUI_RESOURCES "android/content/res/MiuiResources"
#define CLASS_XTYPED_ARRAY   "android/content/res/XResources$XTypedArray"
#define CLASS_ZYGOTE_SERVICE "de/robv/android/xposed/services/ZygoteService"
#define CLASS_FILE_RESULT    "de/robv/android/xposed/services/FileResult"


/////////////////////////////////////////////////////////////////
// Provided by common part, used by runtime-specific implementation
/////////////////////////////////////////////////////////////////
extern bool xposedLoadedSuccessfully;
extern jclass classXposedBridge;

extern int readIntConfig(const char* fileName, int defaultValue);
extern int register_natives_XposedBridge(JNIEnv* env, jclass clazz);


/////////////////////////////////////////////////////////////////
// To be provided by runtime-specific implementation
/////////////////////////////////////////////////////////////////
extern "C" bool xposedInitLib(xposed::XposedShared* shared);
extern void onVmCreated(JNIEnv* env);
extern void logExceptionStackTrace();

extern jboolean callback_XposedBridge_initNative(JNIEnv* env);

extern jint    XposedBridge_getRuntime(JNIEnv* env, jclass clazz);
extern void    XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethodIndirect,
                                             jobject declaredClassIndirect, jint slot, jobject additionalInfoIndirect);
extern void    XposedBridge_setObjectClassNative(JNIEnv* env, jclass clazz, jobject objIndirect, jclass clzIndirect);
extern jobject XposedBridge_cloneToSubclassNative(JNIEnv* env, jclass clazz, jobject objIndirect, jclass clzIndirect);
extern void    XposedBridge_dumpObjectNative(JNIEnv* env, jclass clazz, jobject objIndirect);

#ifdef ART_TARGET
extern jobject XposedBridge_invokeOriginalMethodNative(JNIEnv* env, jclass, jobject javaMethod,
    jint, jobjectArray, jclass, jobject javaReceiver, jobjectArray javaArgs);
#endif

}  // namespace xposed

#endif  // LIBXPOSED_COMMON_H_
