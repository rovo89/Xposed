/*
 * Copyright (c) 2013, rovo89 and Tungstwenty
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DEXSPY_H_
#define DEXSPY_H_

#define ANDROID_SMP 0
#include "Dalvik.h"
#include <list>

namespace android {

#define DEXSPY_DIR "/data/miui/"
#define DEXSPY_JAR DEXSPY_DIR "DexspyInstaller.jar"
#define DEXSPY_JAR_NEWVERSION DEXSPY_DIR "DexspyInstaller.jar.newversion"
#define DEXSPY_LOAD_BLOCKER DEXSPY_DIR "disabled"
#define DEXSPY_CLASS "miui/dexspy/DexspyInstaller"
#define DEXSPY_CLASS_DOTS "miui.dexspy.DexspyInstaller"
#define DEXSPY_VERSION "1.0"

#ifndef ALOGD
#define ALOGD LOGD
#define ALOGE LOGE
#define ALOGI LOGI
#define ALOGV LOGV
#endif

extern bool keepLoadingDexspy;
typedef std::list<Method>::iterator OriginalMethodsIt;

// called directoy by app_process
void dexspyInfo();
bool isDexspyDisabled();
bool dexspyShouldIgnoreCommand(const char* className, int argc, const char* const argv[]);
bool addDexspyToClasspath(bool zygote);
bool dexspyOnVmCreated(JNIEnv* env, const char* className);

// handling hooked methods / helpers
static void dexspyCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self);
static OriginalMethodsIt findOriginalMethod(const Method* method);
static jobject dexspyAddLocalReference(::Thread* self, Object* obj);
static void replaceAsm(void* function, char* newCode, int len);

// JNI methods
static jboolean miui_dexspy_DexspyInstaller_initNative(JNIEnv* env, jclass clazz);
static void miui_dexspy_DexspyInstaller_hookMethodNative(JNIEnv* env, jclass clazz, jobject declaredClassIndirect, jint slot);
static jobject miui_dexspy_DexspyInstaller_invokeOriginalMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod,
            jobjectArray params1, jclass returnType1, jobject thisObject1, jobjectArray args1);
static int register_miui_dexspy_DexspyInstaller(JNIEnv* env);
}

#endif
