/*
 * Xposed enables "god mode" for developers
 */

#define LOG_TAG "Xposed"

#include "xposed.h"

#include <utils/Log.h>
#include <android_runtime/AndroidRuntime.h>

#define private public
#if PLATFORM_SDK_VERSION == 15
#include <utils/ResourceTypes.h>
#else
#include <androidfw/ResourceTypes.h>
#endif
#undef private

#include <stdio.h>
#include <sys/mman.h>

#ifdef WITH_JIT
#include <interp/Jit.h>
#endif

namespace android {

////////////////////////////////////////////////////////////
// variables
////////////////////////////////////////////////////////////
bool keepLoadingXposed = false;
jclass xposedClass = NULL;
jmethodID xposedHandleHookedMethod = NULL;
jclass xresourcesClass = NULL;
jmethodID xresourcesTranslateResId = NULL;
jmethodID xresourcesTranslateAttrId = NULL;
std::list<Method> xposedOriginalMethods;
const char* startClassName = NULL;


////////////////////////////////////////////////////////////
// called directoy by app_process
////////////////////////////////////////////////////////////

bool isXposedDisabled() {
    // is the blocker file present?
    if (access(XPOSED_LOAD_BLOCKER, F_OK) == 0) {
        ALOGE("found %s, not loading Xposed\n", XPOSED_LOAD_BLOCKER);
        return true;
    }
    return false;
}

// ignore the broadcasts by Superuser and SuperSU to avoid spamming the Xposed log
bool xposedShouldIgnoreCommand(const char* className, int argc, const char* const argv[]) {
    if (className == NULL || argc < 4 || strcmp(className, "com.android.commands.am.Am") != 0)
        return false;
        
    if (strcmp(argv[2], "broadcast"))
        return false;
        
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "com.noshufou.android.su.RESULT") == 0
         || strcmp(argv[i], "eu.chainfire.supersu.NativeAccess") == 0)
            return true;
    }
    
    return false;
}

bool addXposedToClasspath(bool zygote) {
    ALOGI("-----------------\n");
    // do we have a new version and are (re)starting zygote? Then load it!
    if (zygote && access(XPOSED_JAR_NEWVERSION, R_OK) == 0) {
        ALOGI("Found new Xposed jar version, activating it\n");
        if (rename(XPOSED_JAR_NEWVERSION, XPOSED_JAR) != 0) {
            ALOGE("could not move %s to %s\n", XPOSED_JAR_NEWVERSION, XPOSED_JAR);
            return false;
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
        ALOGI("Added Xposed (%s) to CLASSPATH.\n", XPOSED_JAR);
        return true;
    } else {
        ALOGE("ERROR: could not access Xposed jar '%s'\n", XPOSED_JAR);
        return false;
    }
}


bool xposedOnVmCreated(JNIEnv* env, const char* className) {
    if (!keepLoadingXposed)
        return false;
        
    startClassName = className;
        
    // disable some access checks
    char asmReturnTrue[] = { 0x01, 0x20, 0x70, 0x47 };
    replaceAsm((void*) &dvmCheckClassAccess,  asmReturnTrue, sizeof(asmReturnTrue));
    replaceAsm((void*) &dvmCheckMethodAccess, asmReturnTrue, sizeof(asmReturnTrue));
    replaceAsm((void*) &dvmCheckFieldAccess,  asmReturnTrue, sizeof(asmReturnTrue));
    replaceAsm((void*) &dvmInSamePackage,     asmReturnTrue, sizeof(asmReturnTrue));

    xposedClass = env->FindClass(XPOSED_CLASS);
    xposedClass = reinterpret_cast<jclass>(env->NewGlobalRef(xposedClass));
    
    if (xposedClass == NULL) {
        ALOGE("Error while loading Xposed class '%s':\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    ALOGI("Found Xposed class '%s', now initializing\n", XPOSED_CLASS);
    register_de_robv_android_xposed_XposedBridge(env);
    register_android_content_res_XResources(env);
    
    xposedHandleHookedMethod = env->GetStaticMethodID(xposedClass, "handleHookedMethod",
        "(Ljava/lang/reflect/Member;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (xposedHandleHookedMethod == NULL) {
        ALOGE("ERROR: could not find method %s.handleHookedMethod(Method, Object, Object[])\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    xresourcesClass = env->FindClass(XRESOURCES_CLASS);
    xresourcesClass = reinterpret_cast<jclass>(env->NewGlobalRef(xresourcesClass));
    if (xresourcesClass == NULL) {
        ALOGE("Error while loading XResources class '%s':\n", XRESOURCES_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    xresourcesTranslateResId = env->GetStaticMethodID(xresourcesClass, "translateResId",
        "(ILandroid/content/res/XResources;Landroid/content/res/Resources;)I");
    if (xresourcesTranslateResId == NULL) {
        ALOGE("ERROR: could not find method %s.translateResId(int, Resources, Resources)\n", XRESOURCES_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    xresourcesTranslateAttrId = env->GetStaticMethodID(xresourcesClass, "translateAttrId",
        "(Ljava/lang/String;Landroid/content/res/XResources;)I");
    if (xresourcesTranslateAttrId == NULL) {
        ALOGE("ERROR: could not find method %s.findAttrId(String, Resources, Resources)\n", XRESOURCES_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    return true;
}


////////////////////////////////////////////////////////////
// handling hooked methods / helpers
////////////////////////////////////////////////////////////

static void xposedCallHandler(const u4* args, JValue* pResult, const Method* method, ::Thread* self) {
    XposedOriginalMethodsIt original = findXposedOriginalMethod(method);
    if (original == xposedOriginalMethods.end()) {
        dvmThrowNoSuchMethodError("could not find Xposed original method - how did you even get here?");
        return;
    }
    
    ThreadStatus oldThreadStatus = self->status;
    JNIEnv* env = self->jniEnv;
    
    // get java.lang.reflect.Method object for original method
    jobject originalReflected = env->ToReflectedMethod(
        (jclass)xposedAddLocalReference(self, original->clazz),
        (jmethodID)method,
        true);
  
    // convert/box arguments
    const char* desc = &method->shorty[1]; // [0] is the return type.
    Object* thisObject = NULL;
    size_t srcIndex = 0;
    size_t dstIndex = 0;
    
    // for non-static methods determine the "this" pointer
    if (!dvmIsStaticMethod(&(*original))) {
        thisObject = (Object*) xposedAddLocalReference(self, (Object*)args[0]);
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
        env->SetObjectArrayElement(argsArray, dstIndex++, xposedAddLocalReference(self, obj));
    }
    
    // call the Java handler function
    jobject resultRef = env->CallStaticObjectMethod(
        xposedClass, xposedHandleHookedMethod, originalReflected, thisObject, argsArray);
        
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
// for certain calls to the JNI environment. almost verbatim copy from Jni.cpp
static jobject xposedAddLocalReference(::Thread* self, Object* obj) {
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

Method* getMethodFromReflectObjWithoutClassInit(jobject jmethod)
{
    Object* obj = dvmDecodeIndirectRef(dvmThreadSelf(), jmethod);
    ClassObject* clazz;
    int slot;

    if (obj->clazz == gDvm.classJavaLangReflectConstructor) {
        clazz = (ClassObject*)dvmGetFieldObject(obj,
                                gDvm.offJavaLangReflectConstructor_declClass);
        slot = dvmGetFieldInt(obj, gDvm.offJavaLangReflectConstructor_slot);
    } else if (obj->clazz == gDvm.classJavaLangReflectMethod) {
        clazz = (ClassObject*)dvmGetFieldObject(obj,
                                gDvm.offJavaLangReflectMethod_declClass);
        slot = dvmGetFieldInt(obj, gDvm.offJavaLangReflectMethod_slot);
    } else {
        assert(false);
        return NULL;
    }

    return dvmSlotToMethod(clazz, slot);
}

static void de_robv_android_xposed_XposedBridge_hookMethodNative(JNIEnv* env, jclass clazz, jobject reflectedMethod) {
    // Usage errors?
    if (reflectedMethod == NULL) {
        dvmThrowIllegalArgumentException("method must not be null");
        return;
    }
    
    // Find the internal representation of the method
    Method* method = getMethodFromReflectObjWithoutClassInit(reflectedMethod);
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
    method->outsSize = 0;
    #ifdef WITH_JIT
    // reset JIT cache
    gDvmJit.codeCacheFull = true;
    #endif
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
    ::Thread* self = dvmThreadSelf();
    Object* thisObject = dvmDecodeIndirectRef(self, thisObject1);
    ArrayObject* args = (ArrayObject*)dvmDecodeIndirectRef(self, args1);
    ArrayObject* params = (ArrayObject*)dvmDecodeIndirectRef(self, params1);
    ClassObject* returnType = (ClassObject*)dvmDecodeIndirectRef(self, returnType1);
    
    // invoke the method
    dvmChangeStatus(self, THREAD_RUNNING);
    Object* result = dvmInvokeMethod(thisObject, method, args, params, returnType, true);
    dvmChangeStatus(self, THREAD_NATIVE);
    
    return xposedAddLocalReference(self, result);
}

static void android_content_res_XResources_rewriteXmlReferencesNative(JNIEnv* env, jclass clazz,
            jint parserPtr, jobject origRes, jobject repRes) {

    ResXMLParser* parser = (ResXMLParser*)parserPtr;
    const ResXMLTree& mTree = parser->mTree;
    uint32_t* mResIds = (uint32_t*)mTree.mResIds;
    ResXMLTree_attrExt* tag;
    int attrCount;
    
    if (parser == NULL)
        return;
    
    do {
        switch (parser->next()) {
            case ResXMLParser::START_TAG:
                tag = (ResXMLTree_attrExt*)parser->mCurExt;
                attrCount = dtohs(tag->attributeCount);
                for (int idx = 0; idx < attrCount; idx++) {
                    ResXMLTree_attribute* attr = (ResXMLTree_attribute*)
                        (((const uint8_t*)tag)
                         + dtohs(tag->attributeStart)
                         + (dtohs(tag->attributeSize)*idx));
                         
                    // find resource IDs for attribute names
                    int32_t attrNameID = parser->getAttributeNameID(idx);
                    // only replace attribute name IDs for app packages
                    if (attrNameID >= 0 && (size_t)attrNameID < mTree.mNumResIds && dtohl(mResIds[attrNameID]) >= 0x7f000000) {
                        size_t attNameLen;
                        const char16_t* attrName = mTree.mStrings.stringAt(attrNameID, &attNameLen);
                        jint attrResID = env->CallStaticIntMethod(xresourcesClass, xresourcesTranslateAttrId,
                            env->NewString((const jchar*)attrName, attNameLen), origRes);
                        if (env->ExceptionCheck())
                            goto leave;
                            
                        mResIds[attrNameID] = htodl(attrResID);
                    }
                    
                    // find original resource IDs for reference values (app packages only)
                    if (attr->typedValue.dataType != Res_value::TYPE_REFERENCE)
                        continue;

                    jint oldValue = dtohl(attr->typedValue.data);
                    if (oldValue < 0x7f000000)
                        continue;
                    
                    jint newValue = env->CallStaticIntMethod(xresourcesClass, xresourcesTranslateResId,
                        oldValue, origRes, repRes);
                    if (env->ExceptionCheck())
                        goto leave;

                    if (newValue != oldValue)
                        attr->typedValue.data = htodl(newValue);
                }
                continue;
            case ResXMLParser::END_DOCUMENT:
            case ResXMLParser::BAD_DOCUMENT:
                goto leave;
            default:
                continue;
        }
    } while (true);
    
    leave:
    parser->restart();
}

static jobject de_robv_android_xposed_XposedBridge_getStartClassName(JNIEnv* env, jclass clazz) {
    return env->NewStringUTF(startClassName);
}

static const JNINativeMethod xposedMethods[] = {
    {"hookMethodNative", "(Ljava/lang/reflect/Member;)V", (void*)de_robv_android_xposed_XposedBridge_hookMethodNative},
    {"invokeOriginalMethodNative", "(Ljava/lang/reflect/Member;[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", (void*)de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative},
    {"getStartClassName", "()Ljava/lang/String;", (void*)de_robv_android_xposed_XposedBridge_getStartClassName},
};

static const JNINativeMethod xresourcesMethods[] = {
    {"rewriteXmlReferencesNative", "(ILandroid/content/res/XResources;Landroid/content/res/Resources;)V", (void*)android_content_res_XResources_rewriteXmlReferencesNative},
};

static int register_de_robv_android_xposed_XposedBridge(JNIEnv* env) {
    return AndroidRuntime::registerNativeMethods(env, XPOSED_CLASS, xposedMethods, NELEM(xposedMethods));
}

static int register_android_content_res_XResources(JNIEnv* env) {
    return AndroidRuntime::registerNativeMethods(env, XRESOURCES_CLASS, xresourcesMethods, NELEM(xresourcesMethods));
}

}

