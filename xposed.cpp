/*
 * Xposed enables "god mode" for developers
 */

#include "xposed.h"

#include <utils/Log.h>
#include <android_runtime/AndroidRuntime.h>

#include <stdio.h>
#include <sys/stat.h>

namespace android {

////////////////////////////////////////////////////////////
// variables
////////////////////////////////////////////////////////////
bool keepLoadingXposed = false;
jclass xposedClass = NULL;
jmethodID xposedHandleHookedMethod = NULL;
std::list<Method> xposedOriginalMethods;


////////////////////////////////////////////////////////////
// called directoy by app_process
////////////////////////////////////////////////////////////

void addXposedToClasspath(bool zygote) {
    // do we have a new version and are (re)starting zygote? Then load it!
    if (zygote && access(XPOSED_JAR_NEWVERSION, R_OK) == 0) {
        LOGI("Found new Xposed jar version, activating it\n");
        if (rename(XPOSED_JAR_NEWVERSION, XPOSED_JAR) != 0) {
            LOGE("could not move %s to %s\n", XPOSED_JAR_NEWVERSION, XPOSED_JAR);
            return;
        }
    }
    if (access(XPOSED_JAR, R_OK) == 0) {
        char* oldClassPath = getenv("CLASSPATH");
        if (oldClassPath == NULL) {
            setenv("CLASSPATH", XPOSED_JAR, 1);
        } else {
            char classPath[4096];
            sprintf(classPath, "%s:%s", XPOSED_JAR, oldClassPath);
            setenv("CLASSPATH", classPath, 1);
        }
        keepLoadingXposed = true;
        LOGI("Added Xposed (%s) to CLASSPATH.\n", XPOSED_JAR);
    } else {
        LOGE("ERROR: could not access Xposed jar '%s'\n", XPOSED_JAR);
    }
}


bool xposedOnVmCreated(JNIEnv* env, const char* loadedClassName) {
    if (!keepLoadingXposed)
        return false;

    xposedClass = env->FindClass(XPOSED_CLASS);
    xposedClass = reinterpret_cast<jclass>(env->NewGlobalRef(xposedClass));
    
    if (xposedClass == NULL) {
        LOGE("Error while loading Xposed class '%s':\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    LOGI("Found Xposed class '%s', now initializing\n", XPOSED_CLASS);
    register_de_robv_android_xposed_XposedBridge(env);
    
    xposedHandleHookedMethod = env->GetStaticMethodID(xposedClass, "handleHookedMethod",
        "(Ljava/lang/reflect/Method;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (xposedHandleHookedMethod == NULL) {
        LOGE("ERROR: could not find method %s.handleHookedMethod(Method, Object, Object[])\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    LOGI("Calling onVmCreated in XposedBridge\n");            
    Method* methodId = (Method*)env->GetStaticMethodID(xposedClass, "onVmCreated", "(Ljava/lang/String;)Z");
    if (methodId == NULL) {
        LOGE("ERROR: could not find method %s.onVmCreated(String)\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    jboolean result = env->CallStaticBooleanMethod(xposedClass, (jmethodID)methodId, env->NewStringUTF(loadedClassName));
    if (env->ExceptionCheck()) {
        LOGE("ERROR: uncaught exception in method %s.onVmCreated(String):\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    return result;
}

void xposedCallStaticVoidMethod(JNIEnv* env, const char* methodName) {
    if (!keepLoadingXposed)
        return;
    
    LOGI("Calling %s in XposedBridge\n", methodName);            
    Method* methodId = (Method*)env->GetStaticMethodID(xposedClass, methodName, "()V");
    if (methodId == NULL) {
        LOGE("ERROR: could not find method %s.%s()\n", XPOSED_CLASS, methodName);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return;
    }
    
    env->CallStaticVoidMethod(xposedClass, (jmethodID)methodId);
    if (env->ExceptionCheck()) {
        LOGE("ERROR: uncaught exception in method %s.%s():\n", XPOSED_CLASS, methodName);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return;
    }
}



////////////////////////////////////////////////////////////
// handling hooked methods
////////////////////////////////////////////////////////////

static void xposedCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self) {
    printf("here I am\n");
    LOGE("here I am");
    
    XposedOriginalMethodsIt original = findXposedOriginalMethod(method);
    if (original == xposedOriginalMethods.end()) {
        dvmThrowNoSuchMethodError("could not find Xposed original method - how did you even get here?");
        return;
    }
    
    assert(method->insSize == original->insSize);
    JNIEnv* env = AndroidRuntime::getJNIEnv();
    
    // get java.lang.reflect.Method object for original method
    jobject originalReflected = env->ToReflectedMethod((jclass)xposedAddLocalReference(original->clazz), (jmethodID)method, true);
  
    // convert/box arguments
    const char* desc = &method->shorty[1]; // [0] is the return type.
    Object* thisObject = NULL;
    u2 numArgs = method->insSize;
    size_t srcIndex = 0;
    size_t dstIndex = 0;
    
    // for non-static methods determine the "this" pointer
    if (!dvmIsStaticMethod(&(*original))) {
        thisObject = (Object*) xposedAddLocalReference((Object*)args[0]);
        srcIndex++;
        numArgs--;
    }
    
    jclass objectClass = env->FindClass("java/lang/Object");
    jobjectArray argsArray = env->NewObjectArray(numArgs, objectClass, NULL);
    
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
        }
        env->SetObjectArrayElement(argsArray, dstIndex++, xposedAddLocalReference(obj));
    }
    
    // call the Java handler function
    jobject resultRef = env->CallStaticObjectMethod(xposedClass, xposedHandleHookedMethod, originalReflected, thisObject, argsArray);
    // exceptions are thrown to the caller
    if (env->ExceptionCheck()) {
        return;
    }
    
    // return result with proper type
    Object* result = dvmDecodeIndirectRef(dvmThreadSelf(), resultRef);
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

    printf("this is me\n");
}


static XposedOriginalMethodsIt findXposedOriginalMethod(const Method* method) {
    if (method == NULL)
        return xposedOriginalMethods.end();

    XposedOriginalMethodsIt it;
    for (XposedOriginalMethodsIt it = xposedOriginalMethods.begin() ; it != xposedOriginalMethods.end(); it++ ) {
        if (strcmp(it->name, method->name) == 0
         && strcmp(it->shorty, method->shorty) == 0
         && strcmp(it->clazz->descriptor, method->clazz->descriptor) == 0) {
            return it;
        }
    }

    return xposedOriginalMethods.end();
}


// work-around to get a reference wrapper to an object so that it can be used
// for certain calls to the JNI environment
static jobject xposedAddLocalReference(Object* obj) {
    if (obj == NULL) {
        return NULL;
    }

    JNIEnv* env = AndroidRuntime::getJNIEnv();
    bool oldWorkaroundValue = gDvmJni.workAroundAppJniBugs;
    gDvmJni.workAroundAppJniBugs = true;
    jobject globalRef = env->NewGlobalRef((jobject)obj);
    gDvmJni.workAroundAppJniBugs = oldWorkaroundValue;
    jobject localRef = env->NewLocalRef(globalRef);
    env->DeleteGlobalRef(globalRef);
    return localRef;
}



////////////////////////////////////////////////////////////
// JNI methods
////////////////////////////////////////////////////////////

static void de_robv_android_xposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod) {
    // Usage errors?
    if (reflectedMethod == NULL) {
        dvmThrowIllegalArgumentException("method must not be null");
        return;
    }
    
    // Find the internal representation of the method
    Method* method = (Method*)env->FromReflectedMethod(reflectedMethod);
    if (method == NULL) {
        dvmThrowNoSuchMethodError("could not get internal representation for method");
        return;
    }
    
    if (findXposedOriginalMethod(method) != xposedOriginalMethods.end()) {
        // already hooked
        return;
    }
    
    // Save a copy of the original method
    xposedOriginalMethods.push_front(*method);

    // Replace method with our own code
    SET_METHOD_FLAG(method, ACC_NATIVE);
    method->nativeFunc = &xposedCallHandler;
    method->registersSize = method->insSize;
}

// simplified copy of Method.invokeNative, but calls the original (non-hooked) method and has no access checks
// used when a method has been hooked
static jobject de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod,
            jobjectArray params1, jclass returnType1, jobject thisObject1, jobjectArray args1) {
    // try to find the original method
    Method* method = (Method*)env->FromReflectedMethod(reflectedMethod);
    XposedOriginalMethodsIt original = findXposedOriginalMethod(method);
    if (original != xposedOriginalMethods.end()) {
        method = &(*original);
    }

    // dereference parameters
    Object* thisObject = dvmDecodeIndirectRef(dvmThreadSelf(), thisObject1);
    ArrayObject* args = (ArrayObject*)dvmDecodeIndirectRef(dvmThreadSelf(), args1);
    ArrayObject* params = (ArrayObject*)dvmDecodeIndirectRef(dvmThreadSelf(), params1);
    ClassObject* returnType = (ClassObject*)dvmDecodeIndirectRef(dvmThreadSelf(), returnType1);
    
    // invoke the method
    Object* result = dvmInvokeMethod(thisObject, method, args, params, returnType, true);
    return xposedAddLocalReference(result);
}

static void de_robv_android_xposed_XposedBridge_setClassModifiersNative(JNIEnv* env, jclass clazz, jobject reflectClass, jint modifiers) {
    ClassObject* classObj = (ClassObject*) dvmDecodeIndirectRef(dvmThreadSelf(), reflectClass);
    LOGE("description %s", classObj->descriptor);
    classObj->accessFlags |= ACC_PUBLIC;
}

static const JNINativeMethod xposedMethods[] = {
    {"hookMethodNative", "(Ljava/lang/reflect/Method;)V", (void*)de_robv_android_xposed_XposedBridge_hookMethodNative},
    {"invokeOriginalMethodNative", "(Ljava/lang/reflect/Method;[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", (void*)de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative},
    {"setClassModifiersNative", "(Ljava/lang/Class;I)V", (void*)de_robv_android_xposed_XposedBridge_setClassModifiersNative},
};

static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env) {
    return AndroidRuntime::registerNativeMethods(env, XPOSED_CLASS, xposedMethods, NELEM(xposedMethods));
}

}

