/**
 * This file includes functions specific to the ART runtime.
 */

#include "xposed_shared.h"
#include "libxposed_common.h"

#include "thread.h"
#include "common_throws.h"
#include "mirror/art_method-inl.h"
#include "mirror/object-inl.h"
#include "mirror/throwable.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "well_known_classes.h"

using namespace art;
namespace xposed {

////////////////////////////////////////////////////////////
// Forward declarations
////////////////////////////////////////////////////////////

void prepareSubclassReplacement(JNIEnv* env, jclass clazz);


////////////////////////////////////////////////////////////
// Library initialization
////////////////////////////////////////////////////////////

/** Called by Xposed's app_process replacement. */
bool xposedInitLib(XposedShared* shared) {
    xposed = shared;
    xposed->onVmCreated = &onVmCreated;
    return true;
}

/** Called very early during VM startup. */
void onVmCreated(JNIEnv* env) {
    // TODO: Handle CLASS_MIUI_RESOURCES?

    jclass classXTypedArray = env->FindClass(CLASS_XTYPED_ARRAY);
    if (classXTypedArray == nullptr) {
        XLOG(ERROR) << "Error while loading XTypedArray class '" CLASS_XTYPED_ARRAY "'";
        logExceptionStackTrace();
        env->ExceptionClear();
        return;
    }
    prepareSubclassReplacement(env, classXTypedArray);

    classXposedBridge = env->FindClass(CLASS_XPOSED_BRIDGE);
    if (classXposedBridge == nullptr) {
        XLOG(ERROR) << "Error while loading Xposed class '" CLASS_XPOSED_BRIDGE "'";
        logExceptionStackTrace();
        env->ExceptionClear();
        return;
    }
    classXposedBridge = reinterpret_cast<jclass>(env->NewGlobalRef(classXposedBridge));
    mirror::ArtMethod::xposed_callback_class = classXposedBridge;

    XLOG(INFO) << "Found Xposed class " CLASS_XPOSED_BRIDGE ", now initializing";
    if (register_natives_XposedBridge(env, classXposedBridge) != JNI_OK) {
        XLOG(ERROR) << "Could not register natives for '" CLASS_XPOSED_BRIDGE "':\n  "
                    << Thread::Current()->GetException(nullptr)->GetDetailMessage()->ToModifiedUtf8();
        env->ExceptionClear();
        return;
    }

    xposedLoadedSuccessfully = true;
}


////////////////////////////////////////////////////////////
// Utility methods
////////////////////////////////////////////////////////////
void logExceptionStackTrace() {
    Thread* self = Thread::Current();
    XLOG(ERROR) << self->GetException(nullptr)->Dump();
}

/** Lay the foundations for XposedBridge.setObjectClassNative() */
void prepareSubclassReplacement(JNIEnv* env, jclass clazz) {
    // clazz is supposed to replace its superclass, so make sure enough memory is allocated
    ScopedObjectAccess soa(env);
    mirror::Class* sub = soa.Decode<mirror::Class*>(clazz);
    mirror::Class* super = sub->GetSuperClass();
    super->SetObjectSize(sub->GetObjectSize());
}


////////////////////////////////////////////////////////////
// JNI methods
////////////////////////////////////////////////////////////

jboolean callback_XposedBridge_initNative(JNIEnv* env) {
    mirror::ArtMethod::xposed_callback_method = env->GetStaticMethodID(classXposedBridge, "handleHookedMethod",
        "(Ljava/lang/reflect/Member;ILjava/lang/Object;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (mirror::ArtMethod::xposed_callback_method == nullptr) {
        XLOG(ERROR) << "ERROR: Could not find method " CLASS_XPOSED_BRIDGE ".handleHookedMethod(Member, int, Object, Object, Object[])";
        logExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }

    return true;
}

void XposedBridge_hookMethodNative(JNIEnv* env, jclass, jobject javaReflectedMethod,
            jobject, jint, jobject javaAdditionalInfo) {
    // Detect usage errors.
    if (javaReflectedMethod == nullptr) {
        ThrowIllegalArgumentException(nullptr, "method must not be null");
        return;
    }

    // Get the ArtMethod of the method to be hooked.
    ScopedObjectAccess soa(env);
    jobject javaArtMethod = env->GetObjectField(javaReflectedMethod,
            WellKnownClasses::java_lang_reflect_AbstractMethod_artMethod);
    mirror::ArtMethod* artMethod = soa.Decode<mirror::ArtMethod*>(javaArtMethod);

    // Hook the method
    artMethod->EnableXposedHook(soa, javaAdditionalInfo);
}

jobject XposedBridge_invokeOriginalMethodNative(JNIEnv* env, jclass, jobject javaMethod,
            jint, jobjectArray, jclass, jobject javaReceiver, jobjectArray javaArgs) {
  ScopedObjectAccess soa(env);
#if PLATFORM_SDK_VERSION >= 21
    return InvokeMethod(soa, javaMethod, javaReceiver, javaArgs, true);
#else
    return InvokeMethod(soa, javaMethod, javaReceiver, javaArgs);
#endif
}

void XposedBridge_setObjectClassNative(JNIEnv* env, jclass, jobject javaObj, jclass javaClazz) {
    ScopedObjectAccess soa(env);
    mirror::Class* clazz = soa.Decode<mirror::Class*>(javaClazz);
#if PLATFORM_SDK_VERSION >= 21
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Class> c(hs.NewHandle(clazz));
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
#else
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(clazz, true, true)) {
#endif
        XLOG(ERROR) << "Could not initialize class " << PrettyClass(clazz);
        return;
    }
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    obj->SetClass(clazz);
}

void XposedBridge_dumpObjectNative(JNIEnv*, jclass, jobject) {
    // TODO Can be useful for debugging
    UNIMPLEMENTED(ERROR|LOG_XPOSED);
}

jobject XposedBridge_cloneToSubclassNative(JNIEnv* env, jclass, jobject javaObject, jclass javaClazz) {
    ScopedObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObject);
    mirror::Class* clazz = soa.Decode<mirror::Class*>(javaClazz);
    mirror::Object* dest = obj->Clone(soa.Self(), clazz->GetObjectSize());
    dest->SetClass(clazz);
    return soa.AddLocalReference<jobject>(dest);
}

jint XposedBridge_getRuntime(JNIEnv* env, jclass clazz) {
    return 2; // RUNTIME_ART
}

}  // namespace xposed
