#ifndef XPOSED_H_
#define XPOSED_H_

#define ANDROID_SMP 0
#include "Dalvik.h"
#include <list>

namespace android {

#define XPOSED_JAR "/data/framework/HelloWorld.apk"
#define XPOSED_JAR_NEWVERSION "/data/framework/HelloWorld.apk.newversion"
#define XPOSED_CLASS "de/robv/android/xposed/XposedBridge"
#define XPOSED_VERSION "0.1"

extern bool keepLoadingXposed;
typedef std::list<Method>::iterator XposedOriginalMethodsIt;

// called directoy by app_process
void addXposedToClasspath(bool zygote);
bool xposedOnVmCreated(JNIEnv* env, const char* loadedClassName);
void xposedCallStaticVoidMethod(JNIEnv* env, const char* methodName);

// handling hooked methods
static void xposedCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
static XposedOriginalMethodsIt findXposedOriginalMethod(const Method* method);
static jobject xposedAddLocalReference(Object* obj);

// JNI methods
static void de_robv_android_xposed_XposedBridge_hookMethod(JNIEnv* env, jclass clazz, jobject reflectedMethod);
static jobject de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod,
            jobjectArray params1, jclass returnType1, jobject thisObject1, jobjectArray args1);
static void de_robv_android_xposed_XposedBridge_setClassModifiers(JNIEnv* env, jclass clazz, jobject reflectClass, jint modifiers);
static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env);
}

#endif  // XPOSED_H_
