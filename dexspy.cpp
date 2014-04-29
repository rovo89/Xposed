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
/*
 * Dexspy enables "god mode" for developers
 */

#define LOG_TAG "Dexspy"

#include "dexspy.h"

#include <utils/Log.h>
#include <android_runtime/AndroidRuntime.h>

#include <stdio.h>
#include <sys/mman.h>
#include <cutils/properties.h>

#ifdef WITH_JIT
#include <interp/Jit.h>
#endif

namespace android {

bool keepLoadingDexspy = false;
jclass dexspyClass = NULL;
jmethodID dexspyHandleHookedMethod = NULL;
std::list<Method> dexspyOriginalMethods;
const char* startClassName = NULL;


void dexspyInfo() {
    char release[PROPERTY_VALUE_MAX];
    char sdk[PROPERTY_VALUE_MAX];
    char manufacturer[PROPERTY_VALUE_MAX];
    char model[PROPERTY_VALUE_MAX];
    char rom[PROPERTY_VALUE_MAX];

    property_get("ro.build.version.release", release, "n/a");
    property_get("ro.build.version.sdk", sdk, "n/a");
    property_get("ro.product.manufacturer", manufacturer, "n/a");
    property_get("ro.product.model", model, "n/a");
    property_get("ro.build.display.id", rom, "n/a");

    ALOGD("Starting Dexspy binary version %s, compiled for SDK %d\n", DEXSPY_VERSION, PLATFORM_SDK_VERSION);
    ALOGD("Phone: %s (%s), Android version %s (SDK %s)\n", model, manufacturer, release, sdk);
    ALOGD("ROM: %s\n", rom);
}

bool isDexspyDisabled() {
    // is the blocker file present?
    if (access(DEXSPY_LOAD_BLOCKER, F_OK) == 0) {
        ALOGE("found %s, not loading Dexspy\n", DEXSPY_LOAD_BLOCKER);
        return true;
    }
    return false;
}

// ignore the broadcasts by various Superuser implementations to avoid spamming the Dexspy log
bool dexspyShouldIgnoreCommand(const char* className, int argc, const char* const argv[]) {
    if (className == NULL || argc < 4 || strcmp(className, "com.android.commands.am.Am") != 0)
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

        char* lastComponent = strrchr(argv[i], '.');
        if (!lastComponent)
            continue;

        if (strcmp(lastComponent, ".RequestActivity") == 0
         || strcmp(lastComponent, ".NotifyActivity") == 0
         || strcmp(lastComponent, ".SuReceiver") == 0)
            mightBeSuperuser = true;
    }

    return false;
}

bool addDexspyToClasspath(bool zygote) {
    return true;
    ALOGI("-----------------\n");
    // do we have a new version and are (re)starting zygote? Then load it!
    if (zygote && access(DEXSPY_JAR_NEWVERSION, R_OK) == 0) {
        ALOGI("Found new Dexspy jar version, activating it\n");
        if (rename(DEXSPY_JAR_NEWVERSION, DEXSPY_JAR) != 0) {
            ALOGE("could not move %s to %s\n", DEXSPY_JAR_NEWVERSION, DEXSPY_JAR);
            return false;
        }
    }
    if (access(DEXSPY_JAR, R_OK) == 0) {
        char* oldClassPath = getenv("CLASSPATH");
        if (oldClassPath == NULL) {
            setenv("CLASSPATH", DEXSPY_JAR, 1);
        } else {
            char classPath[4096];
            sprintf(classPath, "%s:%s", DEXSPY_JAR, oldClassPath);
            setenv("CLASSPATH", classPath, 1);
        }
        ALOGI("Added Dexspy (%s) to CLASSPATH.\n", DEXSPY_JAR);
        return true;
    } else {
        ALOGE("ERROR: could not access Dexspy jar '%s'\n", DEXSPY_JAR);
        return false;
    }
}


bool dexspyOnVmCreated(JNIEnv* env, const char* className) {
    if (!keepLoadingDexspy)
        return false;

    startClassName = className;

    /* The following codes is commented purpose through CTS and avoid to access private fields..
    ** // disable some access checks
    ** char asmReturnTrue[] = { 0x01, 0x20, 0x70, 0x47 };
    ** replaceAsm((void*) &dvmCheckClassAccess,  asmReturnTrue, sizeof(asmReturnTrue));
    ** replaceAsm((void*) &dvmCheckFieldAccess,  asmReturnTrue, sizeof(asmReturnTrue));
    ** replaceAsm((void*) &dvmInSamePackage,     asmReturnTrue, sizeof(asmReturnTrue));
    ** if (access(DEXSPY_DIR "do_not_hook_dvmCheckMethodAccess", F_OK) != 0)
    **        replaceAsm((void*) &dvmCheckMethodAccess, asmReturnTrue, sizeof(asmReturnTrue));
    */

    dexspyClass = env->FindClass(DEXSPY_CLASS);
    dexspyClass = reinterpret_cast<jclass>(env->NewGlobalRef(dexspyClass));

    if (dexspyClass == NULL) {
        ALOGE("Error while loading Dexspy class '%s':\n", DEXSPY_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    ALOGI("Found Dexspy class '%s', now initializing\n", DEXSPY_CLASS);
    register_miui_dexspy_DexspyInstaller(env);
    return true;
}


static void dexspyCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self) {
    OriginalMethodsIt original = findOriginalMethod(method);
    if (original == dexspyOriginalMethods.end()) {
        dvmThrowNoSuchMethodError("could not find Dexspy original method - how did you even get here?");
        return;
    }

    ThreadStatus oldThreadStatus = self->status;
    JNIEnv* env = self->jniEnv;

    // get java.lang.reflect.Method object for original method
    jobject originalReflected = env->ToReflectedMethod(
        (jclass)dexspyAddLocalReference(self, original->clazz),
        (jmethodID)method,
        true);

    // convert/box arguments
    const char* desc = &method->shorty[1]; // [0] is the return type.
    Object* thisObject = NULL;
    size_t srcIndex = 0;
    size_t dstIndex = 0;

    // for non-static methods determine the "this" pointer
    if (!dvmIsStaticMethod(&(*original))) {
        thisObject = (Object*) dexspyAddLocalReference(self, (Object*)args[0]);
        srcIndex++;
    }

    jclass objectClass = env->FindClass("java/lang/Object");
    jobjectArray argsArray = env->NewObjectArray(strlen(method->shorty) - 1, objectClass, NULL);

    while (*desc != '\0') {
        char descChar = *(desc++);
        JValue value;
        Object* obj;

        switch (descChar) {
        case 'Z':
        case 'C':
        case 'F':
        case 'B':
        case 'S':
        case 'I':
            value.i = args[srcIndex++];
            obj = (Object*) dvmBoxPrimitive(value, dvmFindPrimitiveClass(descChar));
            dvmReleaseTrackedAlloc(obj, NULL);
            break;
        case 'D':
        case 'J':
            value.j = dvmGetArgLong(args, srcIndex);
            srcIndex += 2;
            obj = (Object*) dvmBoxPrimitive(value, dvmFindPrimitiveClass(descChar));
            dvmReleaseTrackedAlloc(obj, NULL);
            break;
        case '[':
        case 'L':
            obj  = (Object*) args[srcIndex++];
            break;
        default:
            ALOGE("Unknown method signature description character: %c\n", descChar);
            obj = NULL;
            srcIndex++;
        }
        env->SetObjectArrayElement(argsArray, dstIndex++, dexspyAddLocalReference(self, obj));
    }

    // call the Java handler function
    jobject resultRef = env->CallStaticObjectMethod(
        dexspyClass, dexspyHandleHookedMethod, originalReflected, thisObject, argsArray);

    // exceptions are thrown to the caller
    if (env->ExceptionCheck()) {
        dvmChangeStatus(self, oldThreadStatus);
        return;
    }

    // return result with proper type
    Object* result = dvmDecodeIndirectRef(self, resultRef);
    ClassObject* returnType = dvmGetBoxedReturnType(method);
    if (returnType->primitiveType == PRIM_VOID) {
        // ignored
    } else if (result == NULL) {
        if (dvmIsPrimitiveClass(returnType)) {
            dvmThrowNullPointerException("null result when primitive expected");
        }
        pResult->l = NULL;
    } else {
        if (!dvmUnboxPrimitive(result, returnType, pResult)) {
            dvmThrowClassCastException(result->clazz, returnType);
        }
    }

    // set the thread status back to running. must be done after the last env->...()
    dvmChangeStatus(self, oldThreadStatus);
}


static OriginalMethodsIt findOriginalMethod(const Method* method) {
    if (method == NULL)
        return dexspyOriginalMethods.end();

    for (OriginalMethodsIt it = dexspyOriginalMethods.begin() ; it != dexspyOriginalMethods.end(); it++ ) {
        Method hookedMethod = *it;
        if (dvmCompareMethodNamesAndProtos(&hookedMethod, method) == 0 && strcmp(it->clazz->descriptor, method->clazz->descriptor) == 0) {
            return it;
        }
    }

    return dexspyOriginalMethods.end();
}


// work-around to get a reference wrapper to an object so that it can be used
// for certain calls to the JNI environment. almost verbatim copy from Jni.cpp
static jobject dexspyAddLocalReference(::Thread* self, Object* obj) {
    if (obj == NULL) {
        return NULL;
    }

    IndirectRefTable* pRefTable = &self->jniLocalRefTable;
    void* curFrame = self->interpSave.curFrame;
    u4 cookie = SAVEAREA_FROM_FP(curFrame)->xtra.localRefCookie;
    jobject jobj = (jobject) pRefTable->add(cookie, obj);
    if (UNLIKELY(jobj == NULL)) {
        pRefTable->dump("JNI local");
        ALOGE("Failed adding to JNI local ref table (has %zd entries)", pRefTable->capacity());
        dvmDumpThread(self, false);
        dvmAbort();     // spec says call FatalError; this is equivalent
    }
    if (UNLIKELY(gDvmJni.workAroundAppJniBugs)) {
        // Hand out direct pointers to support broken old apps.
        return reinterpret_cast<jobject>(obj);
    }
    return jobj;
}

static void replaceAsm(void* function, char* newCode, int len) {
    function = (void*)((int)function & ~1);
    void* pageStart = (void*)((int)function & ~(PAGESIZE-1));
    mprotect(pageStart, PAGESIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    memcpy(function, newCode, len);
    mprotect(pageStart, PAGESIZE, PROT_READ | PROT_EXEC);
    __clear_cache(function, (char*)function+len);
}



////////////////////////////////////////////////////////////
// JNI methods
////////////////////////////////////////////////////////////

static jboolean miui_dexspy_DexspyInstaller_initNative(JNIEnv* env, jclass clazz) {
    if (!keepLoadingDexspy) {
        ALOGE("Not initializing Dexspy because of previous errors\n");
        return false;
    }

    dexspyHandleHookedMethod = env->GetStaticMethodID(dexspyClass, "handleHookedMethod",
        "(Ljava/lang/reflect/Member;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (dexspyHandleHookedMethod == NULL) {
        ALOGE("ERROR: could not find method %s.handleHookedMethod(Method, Object, Object[])\n", DEXSPY_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        keepLoadingDexspy = false;
        return false;
    }

    return true;
}

static void miui_dexspy_DexspyInstaller_hookMethodNative(JNIEnv* env, jclass clazz, jobject declaredClassIndirect, jint slot) {
    // Usage errors?
    if (declaredClassIndirect == NULL) {
        dvmThrowIllegalArgumentException("declaredClass must not be null");
        return;
    }

    // Find the internal representation of the method
    ClassObject* declaredClass = (ClassObject*) dvmDecodeIndirectRef(dvmThreadSelf(), declaredClassIndirect);
    Method* method = dvmSlotToMethod(declaredClass, slot);
    if (method == NULL) {
        dvmThrowNoSuchMethodError("could not get internal representation for method");
        return;
    }

    if (findOriginalMethod(method) != dexspyOriginalMethods.end()) {
        ALOGD("why this method already hooked: %s:%s(%s)", method->clazz->descriptor, method->name, method->shorty);
        // already hooked
        return;
    }

    // Save a copy of the original method
    dexspyOriginalMethods.push_front(*method);

    // Replace method with our own code
    SET_METHOD_FLAG(method, ACC_NATIVE);
    method->nativeFunc = &dexspyCallHandler;
    method->registersSize = method->insSize;
    method->outsSize = 0;
    #ifdef WITH_JIT
    // reset JIT cache
    gDvmJit.codeCacheFull = true;
    #endif
}

// simplified copy of Method.invokeNative, but calls the original (non-hooked) method and has no access checks
// used when a method has been hooked
static jobject miui_dexspy_DexspyInstaller_invokeOriginalMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod,
            jobjectArray params1, jclass returnType1, jobject thisObject1, jobjectArray args1) {
    // try to find the original method
    Method* method = (Method*)env->FromReflectedMethod(reflectedMethod);
    OriginalMethodsIt original = findOriginalMethod(method);
    if (original != dexspyOriginalMethods.end()) {
        method = &(*original);
    }

    // dereference parameters
    ::Thread* self = dvmThreadSelf();
    Object* thisObject = dvmDecodeIndirectRef(self, thisObject1);
    ArrayObject* args = (ArrayObject*)dvmDecodeIndirectRef(self, args1);
    ArrayObject* params = (ArrayObject*)dvmDecodeIndirectRef(self, params1);
    ClassObject* returnType = (ClassObject*)dvmDecodeIndirectRef(self, returnType1);

    // invoke the method
    dvmChangeStatus(self, THREAD_RUNNING);
    Object* result = dvmInvokeMethod(thisObject, method, args, params, returnType, true);
    dvmChangeStatus(self, THREAD_NATIVE);

    return dexspyAddLocalReference(self, result);
}

static jobject miui_dexspy_DexspyInstaller_getStartClassName(JNIEnv* env, jclass clazz) {
    return env->NewStringUTF(startClassName);
}

static const JNINativeMethod dexspyMethods[] = {
    {"initNative", "()Z", (void*)miui_dexspy_DexspyInstaller_initNative},
    {"hookMethodNative", "(Ljava/lang/Class;I)V", (void*)miui_dexspy_DexspyInstaller_hookMethodNative},
    {"invokeOriginalMethodNative", "(Ljava/lang/reflect/Member;[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", (void*)miui_dexspy_DexspyInstaller_invokeOriginalMethodNative},
    {"getStartClassName", "()Ljava/lang/String;", (void*)miui_dexspy_DexspyInstaller_getStartClassName},
};

static int register_miui_dexspy_DexspyInstaller(JNIEnv* env) {
    return AndroidRuntime::registerNativeMethods(env, DEXSPY_CLASS, dexspyMethods, NELEM(dexspyMethods));
}

}

