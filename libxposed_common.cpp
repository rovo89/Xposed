/**
 * This file includes functions shared by different runtimes.
 */

#define LOG_TAG "Xposed"

#include "libxposed_common.h"
#include "JNIHelp.h"
#include <ScopedUtfChars.h>

#define private public
#if PLATFORM_SDK_VERSION == 15
#include <utils/ResourceTypes.h>
#else
#include <androidfw/ResourceTypes.h>
#endif
#undef private

namespace xposed {

////////////////////////////////////////////////////////////
// Variables
////////////////////////////////////////////////////////////

bool xposedLoadedSuccessfully = false;
xposed::XposedShared* xposed = NULL;
jclass classXposedBridge = NULL;
static jclass classXResources = NULL;
static jclass classFileResult = NULL;

static jmethodID methodXResourcesTranslateResId = NULL;
static jmethodID methodXResourcesTranslateAttrId = NULL;
static jmethodID constructorFileResult = NULL;


////////////////////////////////////////////////////////////
// Forward declarations
////////////////////////////////////////////////////////////

static int register_natives_XResources(JNIEnv* env, jclass clazz);
static int register_natives_ZygoteService(JNIEnv* env, jclass clazz);


////////////////////////////////////////////////////////////
// Utility methods
////////////////////////////////////////////////////////////

/** Read an integer value from a configuration file. */
int readIntConfig(const char* fileName, int defaultValue) {
    FILE *fp = fopen(fileName, "r");
    if (fp == NULL)
        return defaultValue;

    int result;
    int success = fscanf(fp, "%i", &result);
    fclose(fp);

    return (success >= 1) ? result : defaultValue;
}


////////////////////////////////////////////////////////////
// JNI methods
////////////////////////////////////////////////////////////

jobject XposedBridge_getStartClassName(JNIEnv* env, jclass clazz) {
    return env->NewStringUTF(xposed->startClassName);
}

jboolean XposedBridge_startsSystemServer(JNIEnv* env, jclass clazz) {
    return xposed->startSystemServer;
}

jint XposedBridge_getXposedVersion(JNIEnv* env, jclass clazz) {
    return xposed->xposedVersionInt;
}

jboolean XposedBridge_initNative(JNIEnv* env, jclass clazz) {
    if (!xposedLoadedSuccessfully) {
        ALOGE("Not initializing Xposed because of previous errors");
        return false;
    }

    if (!callback_XposedBridge_initNative(env))
        return false;

    classXResources = env->FindClass(CLASS_XRESOURCES);
    classXResources = reinterpret_cast<jclass>(env->NewGlobalRef(classXResources));
    if (classXResources == NULL) {
        ALOGE("Error while loading XResources class '%s':", CLASS_XRESOURCES);
        logExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    if (register_natives_XResources(env, classXResources) != JNI_OK) {
        ALOGE("Could not register natives for '%s'", CLASS_XRESOURCES);
        env->ExceptionClear();
        return false;
    }

    methodXResourcesTranslateResId = env->GetStaticMethodID(classXResources, "translateResId",
        "(ILandroid/content/res/XResources;Landroid/content/res/Resources;)I");
    if (methodXResourcesTranslateResId == NULL) {
        ALOGE("ERROR: could not find method %s.translateResId(int, Resources, Resources)", CLASS_XRESOURCES);
        logExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    methodXResourcesTranslateAttrId = env->GetStaticMethodID(classXResources, "translateAttrId",
        "(Ljava/lang/String;Landroid/content/res/XResources;)I");
    if (methodXResourcesTranslateAttrId == NULL) {
        ALOGE("ERROR: could not find method %s.findAttrId(String, Resources, Resources)", CLASS_XRESOURCES);
        logExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    jclass zygoteServiceClass = env->FindClass(CLASS_ZYGOTE_SERVICE);
    if (zygoteServiceClass == NULL) {
        ALOGE("Error while loading ZygoteService class '%s':", CLASS_ZYGOTE_SERVICE);
        logExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    if (register_natives_ZygoteService(env, zygoteServiceClass) != JNI_OK) {
        ALOGE("Could not register natives for '%s'", CLASS_ZYGOTE_SERVICE);
        env->ExceptionClear();
        return false;
    }

    classFileResult = env->FindClass(CLASS_FILE_RESULT);
    classFileResult = reinterpret_cast<jclass>(env->NewGlobalRef(classFileResult));
    if (classFileResult == NULL) {
        ALOGE("Error while loading FileResult class '%s':", CLASS_FILE_RESULT);
        logExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    constructorFileResult = env->GetMethodID(classFileResult, "<init>", "(JJ)V");
    if (constructorFileResult == NULL) {
        ALOGE("ERROR: could not find constructor %s(long, long)", CLASS_FILE_RESULT);
        logExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    return true;
}

void XResources_rewriteXmlReferencesNative(JNIEnv* env, jclass clazz,
            jlong parserPtr, jobject origRes, jobject repRes) {

    using namespace android;

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
                        jint attrResID = env->CallStaticIntMethod(classXResources, methodXResourcesTranslateAttrId,
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

                    jint newValue = env->CallStaticIntMethod(classXResources, methodXResourcesTranslateResId,
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


jboolean ZygoteService_checkFileAccess(JNIEnv* env, jclass clazz, jstring filenameJ, jint mode) {
#if XPOSED_WITH_SELINUX
    ScopedUtfChars filename(env, filenameJ);
    return xposed->zygoteservice_accessFile(filename.c_str(), mode) == 0;
#else  // XPOSED_WITH_SELINUX
    return false;
#endif  // XPOSED_WITH_SELINUX
}

jobject ZygoteService_statFile(JNIEnv* env, jclass clazz, jstring filenameJ) {
#if XPOSED_WITH_SELINUX
    ScopedUtfChars filename(env, filenameJ);

    struct stat st;
    int result = xposed->zygoteservice_statFile(filename.c_str(), &st);
    if (result != 0) {
        if (errno == ENOENT) {
            jniThrowExceptionFmt(env, "java/io/FileNotFoundException", "No such file or directory: %s", filename.c_str());
        } else {
            jniThrowExceptionFmt(env, "java/io/IOException", "%s while reading %s", strerror(errno), filename.c_str());
        }
        return NULL;
    }

    return env->NewObject(classFileResult, constructorFileResult, (jlong) st.st_size, (jlong) st.st_mtime);
#else  // XPOSED_WITH_SELINUX
    return NULL;
#endif  // XPOSED_WITH_SELINUX
}

jbyteArray ZygoteService_readFile(JNIEnv* env, jclass clazz, jstring filenameJ) {
#if XPOSED_WITH_SELINUX
    ScopedUtfChars filename(env, filenameJ);

    int bytesRead = 0;
    char* content = xposed->zygoteservice_readFile(filename.c_str(), &bytesRead);
    if (content == NULL) {
        if (errno == ENOENT) {
            jniThrowExceptionFmt(env, "java/io/FileNotFoundException", "No such file or directory: %s", filename.c_str());
        } else {
            jniThrowExceptionFmt(env, "java/io/IOException", "%s while reading %s", strerror(errno), filename.c_str());
        }
        return NULL;
    }

    jbyteArray ret = env->NewByteArray(bytesRead);
    if (ret != NULL) {
        jbyte* arrptr = (jbyte*)env->GetPrimitiveArrayCritical(ret, 0);
        if (arrptr) {
            memcpy(arrptr, content, bytesRead);
            env->ReleasePrimitiveArrayCritical(ret, arrptr, 0);
        }
    }

    free(content);
    return ret;
#else  // XPOSED_WITH_SELINUX
    return NULL;
#endif  // XPOSED_WITH_SELINUX
}

////////////////////////////////////////////////////////////
// JNI methods registrations
////////////////////////////////////////////////////////////

int register_natives_XposedBridge(JNIEnv* env, jclass clazz) {
    const JNINativeMethod methods[] = {
        NATIVE_METHOD(XposedBridge, getStartClassName, "()Ljava/lang/String;"),
        NATIVE_METHOD(XposedBridge, getRuntime, "()I"),
        NATIVE_METHOD(XposedBridge, startsSystemServer, "()Z"),
        NATIVE_METHOD(XposedBridge, getXposedVersion, "()I"),
        NATIVE_METHOD(XposedBridge, initNative, "()Z"),
        NATIVE_METHOD(XposedBridge, hookMethodNative, "(Ljava/lang/reflect/Member;Ljava/lang/Class;ILjava/lang/Object;)V"),
#ifdef ART_TARGET
        NATIVE_METHOD(XposedBridge, invokeOriginalMethodNative,
            "!(Ljava/lang/reflect/Member;I[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"),
#endif
        NATIVE_METHOD(XposedBridge, setObjectClassNative, "(Ljava/lang/Object;Ljava/lang/Class;)V"),
        NATIVE_METHOD(XposedBridge, dumpObjectNative, "(Ljava/lang/Object;)V"),
        NATIVE_METHOD(XposedBridge, cloneToSubclassNative, "(Ljava/lang/Object;Ljava/lang/Class;)Ljava/lang/Object;"),
    };
    return env->RegisterNatives(clazz, methods, NELEM(methods));
}

int register_natives_XResources(JNIEnv* env, jclass clazz) {
    const JNINativeMethod methods[] = {
        NATIVE_METHOD(XResources, rewriteXmlReferencesNative, "(JLandroid/content/res/XResources;Landroid/content/res/Resources;)V"),
    };
    return env->RegisterNatives(clazz, methods, NELEM(methods));
}

int register_natives_ZygoteService(JNIEnv* env, jclass clazz) {
    const JNINativeMethod methods[] = {
        NATIVE_METHOD(ZygoteService, checkFileAccess, "(Ljava/lang/String;I)Z"),
        NATIVE_METHOD(ZygoteService, statFile, "(Ljava/lang/String;)L" CLASS_FILE_RESULT ";"),
        NATIVE_METHOD(ZygoteService, readFile, "(Ljava/lang/String;)[B"),
    };
    return env->RegisterNatives(clazz, methods, NELEM(methods));
}

}  // namespace xposed
