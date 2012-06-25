/*
 * Xposed enables "god mode" for developers
 */

#define LOG_TAG "Xposed"

#include "xposed.h"

#include <utils/Log.h>
#include <android_runtime/AndroidRuntime.h>

#define private public
#include <utils/ResourceTypes.h>
#undef private

#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <bzlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <cutils/properties.h>
#include <sys/mount.h>

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
        LOGE("found %s, not loading Xposed\n", XPOSED_LOAD_BLOCKER);
        return true;
    }
    return false;
}

bool maybeReplaceLibs(bool zygote) {
    if (!zygote)
        return true;
    
    char propReplaced[PROPERTY_VALUE_MAX];
    char propTestMode[PROPERTY_VALUE_MAX];
    struct stat sb;
    
    property_get("xposed.libs.replaced", propReplaced, "0");
    property_get("xposed.libs.testmode", propTestMode, "0");
    bool testmode = (propTestMode[0] == '1');
    
    if (propReplaced[0] == '0' || testmode) {
        property_set("xposed.libs.replaced", "1");
        property_set("xposed.libs.testmode", "0");
    
        // only continue if the lib dir exists
        bool alwaysDirExists = (stat(XPOSED_LIBS_ALWAYS, &sb) == 0 && S_ISDIR(sb.st_mode));
        bool testDirExists = (stat(XPOSED_LIBS_TESTMODE, &sb) == 0 && S_ISDIR(sb.st_mode));
        
        if (!alwaysDirExists && !(testmode && testDirExists))
            return true;
        
        LOGI("Copying native libraries into /vendor/lib");
        
        // try remounting the file system root r/w
        int ret = mount("rootfs", "/", "rootfs", MS_REMOUNT, NULL);
        if (ret != 0) {
            LOGE("Could not mount \"/\" r/w, error %d", ret);
            return true;
        }
        
        // copy libs
        mkdir("/vendor/lib/", 0755);
        if (alwaysDirExists)
            system("cp -a " XPOSED_LIBS_ALWAYS "* /vendor/lib/");
        if (testmode && testDirExists)
            system("cp -a " XPOSED_LIBS_TESTMODE "* /vendor/lib/");

        // restart zygote
        property_set("ctl.restart", "surfaceflinger");
        property_set("ctl.restart", "zygote");
        exit(0);
    }
    return true;
}

bool addXposedToClasspath(bool zygote) {
    LOGI("-----------------\n");
    // do we have a new version and are (re)starting zygote? Then load it!
    if (zygote && access(XPOSED_JAR_NEWVERSION, R_OK) == 0) {
        LOGI("Found new Xposed jar version, activating it\n");
        if (rename(XPOSED_JAR_NEWVERSION, XPOSED_JAR) != 0) {
            LOGE("could not move %s to %s\n", XPOSED_JAR_NEWVERSION, XPOSED_JAR);
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
        LOGI("Added Xposed (%s) to CLASSPATH.\n", XPOSED_JAR);
        return true;
    } else {
        LOGE("ERROR: could not access Xposed jar '%s'\n", XPOSED_JAR);
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
        LOGE("Error while loading Xposed class '%s':\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    LOGI("Found Xposed class '%s', now initializing\n", XPOSED_CLASS);
    register_de_robv_android_xposed_XposedBridge(env);
    register_android_content_res_XResources(env);
    
    xposedHandleHookedMethod = env->GetStaticMethodID(xposedClass, "handleHookedMethod",
        "(Ljava/lang/reflect/Member;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    if (xposedHandleHookedMethod == NULL) {
        LOGE("ERROR: could not find method %s.handleHookedMethod(Method, Object, Object[])\n", XPOSED_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    xresourcesClass = env->FindClass(XRESOURCES_CLASS);
    xresourcesClass = reinterpret_cast<jclass>(env->NewGlobalRef(xresourcesClass));
    if (xresourcesClass == NULL) {
        LOGE("Error while loading XResources class '%s':\n", XRESOURCES_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    xresourcesTranslateResId = env->GetStaticMethodID(xresourcesClass, "translateResId",
        "(ILandroid/content/res/XResources;Landroid/content/res/Resources;)I");
    if (xresourcesTranslateResId == NULL) {
        LOGE("ERROR: could not find method %s.translateResId(int, Resources, Resources)\n", XRESOURCES_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    xresourcesTranslateAttrId = env->GetStaticMethodID(xresourcesClass, "translateAttrId",
        "(Ljava/lang/String;Landroid/content/res/XResources;)I");
    if (xresourcesTranslateAttrId == NULL) {
        LOGE("ERROR: could not find method %s.findAttrId(String, Resources, Resources)\n", XRESOURCES_CLASS);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        return false;
    }
    
    return true;
}

void xposedCallStaticVoidMethod(JNIEnv* env, const char* methodName) {
    if (!keepLoadingXposed)
        return;
    
    ::Thread* self = dvmThreadSelf();
    ThreadStatus oldThreadStatus = self->status;
    //LOGI("Calling %s in XposedBridge\n", methodName);            
    Method* methodId = (Method*)env->GetStaticMethodID(xposedClass, methodName, "()V");
    if (methodId == NULL) {
        LOGE("ERROR: could not find method %s.%s()\n", XPOSED_CLASS, methodName);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        dvmChangeStatus(self, oldThreadStatus);
        return;
    }
    
    env->CallStaticVoidMethod(xposedClass, (jmethodID)methodId);
    if (env->ExceptionCheck()) {
        LOGE("ERROR: uncaught exception in method %s.%s():\n", XPOSED_CLASS, methodName);
        dvmLogExceptionStackTrace();
        env->ExceptionClear();
        dvmChangeStatus(self, oldThreadStatus);
        return;
    }
    dvmChangeStatus(self, oldThreadStatus);
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
    jobjectArray argsArray = env->NewObjectArray(dvmComputeMethodArgsSize(method), objectClass, NULL);
    
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
            LOGE("Unknown method signature description character: %c\n", descChar);
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
        LOGE("Failed adding to JNI local ref table (has %zd entries)", pRefTable->capacity());
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

int memcpyToProcess(pid_t pid, void* dest, const void* src, size_t n) {
    long *d = (long*) dest;
    long *s = (long*) src;
    
	if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
	    LOGE("could not attach to process %d: %s", pid, strerror(errno));
	    return 1;
    }
	   
	waitpid(pid, 0, 0);

    for (uint i = 0; i < n / sizeof(long); i++) {
        if (ptrace(PTRACE_POKETEXT, pid, d+i, (void*)s[i]) == -1) {
	        LOGE("could not write memory to process %d (%p): %s", pid, d+i, strerror(errno));
	        return 1;
        }
    }

	if (ptrace(PTRACE_DETACH, pid, NULL, NULL) == -1) {
	    LOGE("could not detach from process %d: %s", pid, strerror(errno));
	    return 1;
    }
    
    return 0;
}

// code copied from bspatch (http://www.daemonology.net/bsdiff/) and adjusted for in-memory replacement
static off_t offtin(u_char *buf)
{
	off_t y;

	y=buf[7]&0x7F;
	y=y*256;y+=buf[6];
	y=y*256;y+=buf[5];
	y=y*256;y+=buf[4];
	y=y*256;y+=buf[3];
	y=y*256;y+=buf[2];
	y=y*256;y+=buf[1];
	y=y*256;y+=buf[0];

	if(buf[7]&0x80) y=-y;

	return y;
}

/* main changes:
    - patched code is written to memory instead of file
    - patching memory of other processes is supported
    - only the first x bytes are patched, were x=min(size in patch header,original size,libsize from memory map)
    - it is no error when the target size is larger than x, patching simply stops after x bytes
*/
int patchNativeLibrary(const char* libname, const char* patchfile, pid_t pid, u_char* libbase, ssize_t libsize)
{
	FILE * f, * cpf, * dpf, * epf;
	BZFILE * cpfbz2, * dpfbz2, * epfbz2;
	int cbz2err, dbz2err, ebz2err;
	int fd;
	ssize_t oldsize,newsize,orig_newsize;
	ssize_t bzctrllen,bzdatalen;
	u_char header[32],buf[8];
	u_char *old, *newbytes;
	off_t oldpos,newpos;
	off_t ctrl[3];
	off_t lenread;
	off_t i;

	/* Open patch file */
	if ((f = fopen(patchfile, "r")) == NULL)
		return 1;

	/*
	File format:
		0	8	"BSDIFF40"
		8	8	X
		16	8	Y
		24	8	sizeof(newfile)
		32	X	bzip2(control block)
		32+X	Y	bzip2(diff block)
		32+X+Y	???	bzip2(extra block)
	with control block a set of triples (x,y,z) meaning "add x bytes
	from oldfile to x bytes from the diff block; copy y bytes from the
	extra block; seek forwards in oldfile by z bytes".
	*/

	/* Read header */
	if (fread(header, 1, 32, f) < 32)
		return 1;

	/* Check for appropriate magic */
	if (memcmp(header, "BSDIFF40", 8) != 0)
		return 1;

	/* Read lengths from header */
	bzctrllen=offtin(header+8);
	bzdatalen=offtin(header+16);
	newsize=orig_newsize=offtin(header+24);
	if((bzctrllen<0) || (bzdatalen<0) || (newsize<0))
		return 1;

	/* Close patch file and re-open it via libbzip2 at the right places */
	if ((fclose(f))
	
	|| ((cpf = fopen(patchfile, "r")) == NULL)
	|| (fseeko(cpf, 32, SEEK_SET))
	|| ((cpfbz2 = BZ2_bzReadOpen(&cbz2err, cpf, 0, 0, NULL, 0)) == NULL)
	
	|| ((dpf = fopen(patchfile, "r")) == NULL)
	|| (fseeko(dpf, 32 + bzctrllen, SEEK_SET))
	|| ((dpfbz2 = BZ2_bzReadOpen(&dbz2err, dpf, 0, 0, NULL, 0)) == NULL)
	
	|| ((epf = fopen(patchfile, "r")) == NULL)
	|| (fseeko(epf, 32 + bzctrllen + bzdatalen, SEEK_SET))
	|| ((epfbz2 = BZ2_bzReadOpen(&ebz2err, epf, 0, 0, NULL, 0)) == NULL))
    	return 1;

	
	if(((fd=open(libname,O_RDONLY,0))<0) ||
		((oldsize=lseek(fd,0,SEEK_END))==-1) ||
		((old=(u_char*)malloc(oldsize+1))==NULL) ||
		(lseek(fd,0,SEEK_SET)!=0) ||
		(read(fd,old,oldsize)!=oldsize) ||
		(close(fd)==-1))
		return 1;
		
	if (newsize > libsize)
	    newsize = libsize;
    
    if ((newbytes=(u_char*)malloc(newsize+1))==NULL)
	    return 1;
	
	memcpy(newbytes, old, newsize);

	oldpos=0;newpos=0;
	while(newpos<newsize) {
		/* Read control data */
		for(i=0;i<=2;i++) {
			lenread = BZ2_bzRead(&cbz2err, cpfbz2, buf, 8);
			if ((lenread < 8) || ((cbz2err != BZ_OK) &&
			    (cbz2err != BZ_STREAM_END)))
				return 1;
			ctrl[i]=offtin(buf);
		};
		
		/* Sanity-check */
		if(newpos+ctrl[0]>newsize)
			ctrl[0] = newsize - newpos;

		/* Read diff string */
		lenread = BZ2_bzRead(&dbz2err, dpfbz2, newbytes+ newpos, ctrl[0]);
		if ((lenread < ctrl[0]) ||
		    ((dbz2err != BZ_OK) && (dbz2err != BZ_STREAM_END)))
			return 1;

		/* Add old data to diff string */
		for(i=0;i<ctrl[0];i++)
			if((oldpos+i>=0) && (oldpos+i<oldsize))
				newbytes[newpos+i]+=old[oldpos+i];

		/* Adjust pointers */
		newpos+=ctrl[0];
		oldpos+=ctrl[0];

		/* Sanity-check */
		if(newpos+ctrl[1]>newsize)
			ctrl[1] = newsize - newpos;

		/* Read extra string */
		lenread = BZ2_bzRead(&ebz2err, epfbz2, newbytes + newpos, ctrl[1]);
		if ((lenread < ctrl[1]) ||
		    ((ebz2err != BZ_OK) && (ebz2err != BZ_STREAM_END)))
			return 1;

		/* Adjust pointers */
		newpos+=ctrl[1];
		oldpos+=ctrl[2];
	};

	/* Clean up the bzip2 reads */
	BZ2_bzReadClose(&cbz2err, cpfbz2);
	BZ2_bzReadClose(&dbz2err, dpfbz2);
	BZ2_bzReadClose(&ebz2err, epfbz2);
	if (fclose(cpf) || fclose(dpf) || fclose(epf))
	    return 1;

	/* Write patched memory content */
	if (pid == 0) {
        void* pageStart = (void*)((int)libbase & ~(PAGESIZE-1));
        mprotect(pageStart, newsize, PROT_READ | PROT_WRITE | PROT_EXEC);
        memcpy(libbase, newbytes, newsize);
    } else {
        if (memcpyToProcess(pid, libbase, newbytes, newsize))
            return 1;
    }
    
	free(newbytes);
    
	return 0;
}
// end of bspatch code

static jboolean de_robv_android_xposed_XposedBridge_patchNativeLibrary(JNIEnv* env, jclass clazz, jstring library, jbyteArray patch, jint pid, jlong libBase, jlong libSize) {
    int returncode = 0;
    jbyte* patchBytes;
    ssize_t written;
    char tmpPath[256];
    const char* libraryPath;
    FILE* tmpFile = NULL;
    
    ssize_t size = env->GetArrayLength(patch);
    if (size == 0)
        goto leave;
        
    // write byte array to temporary file
    strcpy(tmpPath, "/data/xposed/tmp/xposed.libpatch.XXXXXX");
    tmpFile = fdopen(mkstemp(tmpPath), "w");
    if (tmpFile == NULL) {
        LOGE("Could not create a temporary file %s", tmpPath);
        goto leave;
    }

    patchBytes = env->GetByteArrayElements(patch, NULL);
    written = fwrite(patchBytes, 1, size, tmpFile);
    if (written != size) {
        LOGE("Could only write %d of %d bytes to %s in patchNativeLibrary", (int)written, (int)size, tmpPath);
        fclose(tmpFile);
        goto leave;
    }
    fclose(tmpFile);
    
    env->ReleaseByteArrayElements(patch, patchBytes, 0);
    libraryPath = env->GetStringUTFChars(library, NULL);
    returncode = patchNativeLibrary(libraryPath, tmpPath, pid, (u_char*)libBase, (ssize_t)libSize);
    env->ReleaseStringUTFChars(library, libraryPath);

leave:
    unlink(tmpPath);
    return returncode == 0;
}


static const JNINativeMethod xposedMethods[] = {
    {"hookMethodNative", "(Ljava/lang/reflect/Member;)V", (void*)de_robv_android_xposed_XposedBridge_hookMethodNative},
    {"invokeOriginalMethodNative", "(Ljava/lang/reflect/Member;[Ljava/lang/Class;Ljava/lang/Class;Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", (void*)de_robv_android_xposed_XposedBridge_invokeOriginalMethodNative},
    {"getStartClassName", "()Ljava/lang/String;", (void*)de_robv_android_xposed_XposedBridge_getStartClassName},
    {"patchNativeLibrary", "(Ljava/lang/String;[BIJJ)Z", (void*)de_robv_android_xposed_XposedBridge_patchNativeLibrary},
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

