/*
 * Main entry of app process.
 *
 * Starts the interpreted runtime, then starts up the application.
 *
 */

#define LOG_TAG "appproc"

#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <utils/Log.h>
#include <cutils/process_name.h>
#include <cutils/memory.h>
#define private public
#include <android_runtime/AndroidRuntime.h>
#undef private
#include <sys/stat.h>

#include <stdio.h>
#include <unistd.h>
#include "xposed.h"

namespace android {

void app_usage()
{
    fprintf(stderr,
        "Usage: app_process [java-options] cmd-dir start-class-name [options]\n");
    fprintf(stderr, "   with Xposed support (version " XPOSED_VERSION ")\n");
}

class AppRuntime : public AndroidRuntime
{
public:
    AppRuntime()
        : mParentDir(NULL)
        , mClassName(NULL)
#if PLATFORM_SDK_VERSION >= 14
        , mClass(NULL)
#endif
        , mArgC(0)
        , mArgV(NULL)
    {
    }

#if 0
    // this appears to be unused
    const char* getParentDir() const
    {
        return mParentDir;
    }
#endif

    const char* getClassName() const
    {
        return mClassName;
    }

#if PLATFORM_SDK_VERSION < 14
/*
 * We just want failed write() calls to just return with an error.
 */
static void blockSigpipe()
{
    sigset_t mask;

    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
        LOGW("WARNING: SIGPIPE not blocked\n");
}

static int hasDir(const char* dir)
{
    struct stat s;
    int res = stat(dir, &s);
    if (res == 0) {
        return S_ISDIR(s.st_mode);
    }
    return 0;
}

/*
 * Start the Android runtime.  This involves starting the virtual machine
 * and calling the "static void main(String[] args)" method in the class
 * named by "className".
 */
void start(const char* className, const bool startSystemServer)
{
    LOGD("\n>>>>>> AndroidRuntime START %s <<<<<<\n",
            className != NULL ? className : "(unknown)");

    char* slashClassName = NULL;
    char* cp;
    JNIEnv* env;

    blockSigpipe();

    /*
     * 'startSystemServer == true' means runtime is obsolete and not run from
     * init.rc anymore, so we print out the boot start event here.
     */
    if (startSystemServer) {
        /* track our progress through the boot sequence */
        const int LOG_BOOT_PROGRESS_START = 3000;
        LOG_EVENT_LONG(LOG_BOOT_PROGRESS_START,
                       ns2ms(systemTime(SYSTEM_TIME_MONOTONIC)));
    }

    const char* rootDir = getenv("ANDROID_ROOT");
    if (rootDir == NULL) {
        rootDir = "/system";
        if (!hasDir("/system")) {
            LOG_FATAL("No root directory specified, and /android does not exist.");
            goto bail;
        }
        setenv("ANDROID_ROOT", rootDir, 1);
    }

    //const char* kernelHack = getenv("LD_ASSUME_KERNEL");
    //LOGD("Found LD_ASSUME_KERNEL='%s'\n", kernelHack);

    /* start the virtual machine */
    if (startVm(&mJavaVM, &env) != 0)
        goto bail;

    keepLoadingXposed = xposedOnVmCreated(env, mClassName);

    /*
     * Register android functions.
     */
    if (startReg(env) < 0) {
        LOGE("Unable to register all android natives\n");
        goto bail;
    }

    /*
     * We want to call main() with a String array with arguments in it.
     * At present we only have one argument, the class name.  Create an
     * array to hold it.
     */
    jclass stringClass;
    jobjectArray strArray;
    jstring classNameStr;
    jstring startSystemServerStr;

    stringClass = env->FindClass("java/lang/String");
    assert(stringClass != NULL);
    strArray = env->NewObjectArray(2, stringClass, NULL);
    assert(strArray != NULL);
    classNameStr = env->NewStringUTF(className);
    assert(classNameStr != NULL);
    env->SetObjectArrayElement(strArray, 0, classNameStr);
    startSystemServerStr = env->NewStringUTF(startSystemServer ?
                                                 "true" : "false");
    env->SetObjectArrayElement(strArray, 1, startSystemServerStr);

    /*
     * Start VM.  This thread becomes the main thread of the VM, and will
     * not return until the VM exits.
     */
    jclass startClass;
    jmethodID startMeth;

    slashClassName = strdup(className);
    for (cp = slashClassName; *cp != '\0'; cp++)
        if (*cp == '.')
            *cp = '/';

    startClass = env->FindClass(slashClassName);
    if (startClass == NULL) {
        LOGE("JavaVM unable to locate class '%s'\n", slashClassName);
        /* keep going */
    } else {
        startMeth = env->GetStaticMethodID(startClass, "main",
            "([Ljava/lang/String;)V");
        if (startMeth == NULL) {
            LOGE("JavaVM unable to find main() in '%s'\n", className);
            /* keep going */
        } else {
            env->CallStaticVoidMethod(startClass, startMeth, strArray);
#if 0
            if (env->ExceptionCheck())
                LOGE("there is exception");
#endif
        }
    }

    LOGD("Shutting down VM\n");
    if (mJavaVM->DetachCurrentThread() != JNI_OK)
        LOGW("Warning: unable to detach main thread\n");
    if (mJavaVM->DestroyJavaVM() != 0)
        LOGW("Warning: VM did not shut down cleanly\n");

bail:
    free(slashClassName);
}
#else
    virtual void onVmCreated(JNIEnv* env)
    {
        keepLoadingXposed = xposedOnVmCreated(env, mClassName);

        if (mClassName == NULL) {
            return; // Zygote. Nothing to do here.
        }

        /*
         * This is a little awkward because the JNI FindClass call uses the
         * class loader associated with the native method we're executing in.
         * If called in onStarted (from RuntimeInit.finishInit because we're
         * launching "am", for example), FindClass would see that we're calling
         * from a boot class' native method, and so wouldn't look for the class
         * we're trying to look up in CLASSPATH. Unfortunately it needs to,
         * because the "am" classes are not boot classes.
         *
         * The easiest fix is to call FindClass here, early on before we start
         * executing boot class Java code and thereby deny ourselves access to
         * non-boot classes.
         */
        char* slashClassName = toSlashClassName(mClassName);
        mClass = env->FindClass(slashClassName);
        if (mClass == NULL) {
            ALOGE("ERROR: could not find class '%s'\n", mClassName);
        }
        free(slashClassName);

        mClass = reinterpret_cast<jclass>(env->NewGlobalRef(mClass));
    }
#endif

    virtual void onStarted()
    {
        sp<ProcessState> proc = ProcessState::self();
        ALOGV("App process: starting thread pool.\n");
        proc->startThreadPool();

        AndroidRuntime* ar = AndroidRuntime::getRuntime();
#if PLATFORM_SDK_VERSION < 14
        ar->callMain(mClassName, mArgC, mArgV);
#else
        ar->callMain(mClassName, mClass, mArgC, mArgV);
#endif

        IPCThreadState::self()->stopProcess();
    }

    virtual void onZygoteInit()
    {
        sp<ProcessState> proc = ProcessState::self();
        ALOGV("App process: starting thread pool.\n");
        proc->startThreadPool();
    }

    virtual void onExit(int code)
    {
        if (mClassName == NULL) {
            // if zygote
            IPCThreadState::self()->stopProcess();
        }

        AndroidRuntime::onExit(code);
    }


    const char* mParentDir;
    const char* mClassName;
#if PLATFORM_SDK_VERSION >= 14
    jclass mClass;
#endif
    int mArgC;
    const char* const* mArgV;
};

}

using namespace android;

/*
 * sets argv0 to as much of newArgv0 as will fit
 */
static void setArgv0(const char *argv0, const char *newArgv0)
{
    strlcpy(const_cast<char *>(argv0), newArgv0, strlen(argv0));
}

int main(int argc, const char* const argv[])
{
    if (argc == 2 && strcmp(argv[1], "--xposedversion") == 0) {
        printf("Xposed version: " XPOSED_VERSION "\n");
        return 0;
    }

    // These are global variables in ProcessState.cpp
    mArgC = argc;
    mArgV = argv;

    mArgLen = 0;
    for (int i=0; i<argc; i++) {
        mArgLen += strlen(argv[i]) + 1;
    }
    mArgLen--;

    AppRuntime runtime;
    const char* argv0 = argv[0];

    // Process command line arguments
    // ignore argv[0]
    argc--;
    argv++;

    // Everything up to '--' or first non '-' arg goes to the vm

    int i = runtime.addVmArguments(argc, argv);

    // Parse runtime arguments.  Stop at first unrecognized option.
    bool zygote = false;
    bool startSystemServer = false;
    bool application = false;
    const char* parentDir = NULL;
    const char* niceName = NULL;
    const char* className = NULL;
    while (i < argc) {
        const char* arg = argv[i++];
        if (!parentDir) {
            parentDir = arg;
        } else if (strcmp(arg, "--zygote") == 0) {
            zygote = true;
            niceName = "zygote";
        } else if (strcmp(arg, "--start-system-server") == 0) {
            startSystemServer = true;
        } else if (strcmp(arg, "--application") == 0) {
            application = true;
        } else if (strncmp(arg, "--nice-name=", 12) == 0) {
            niceName = arg + 12;
        } else {
            className = arg;
            break;
        }
    }

    if (niceName && *niceName) {
        setArgv0(argv0, niceName);
        set_process_name(niceName);
    }

    runtime.mParentDir = parentDir;
    
    xposedInfo();
    keepLoadingXposed = !isXposedDisabled() && !xposedShouldIgnoreCommand(className, argc, argv) && addXposedToClasspath(zygote);

    if (zygote) {
        runtime.start(keepLoadingXposed ? XPOSED_CLASS_DOTS : "com.android.internal.os.ZygoteInit",
#if PLATFORM_SDK_VERSION < 14
                startSystemServer);
#else
                startSystemServer ? "start-system-server" : "");
#endif
    } else if (className) {
        // Remainder of args get passed to startup class main()
        runtime.mClassName = className;
        runtime.mArgC = argc - i;
        runtime.mArgV = argv + i;
        runtime.start(keepLoadingXposed ? XPOSED_CLASS_DOTS : "com.android.internal.os.RuntimeInit",
#if PLATFORM_SDK_VERSION < 14
                false);
#else
                application ? "application" : "tool");
#endif
    } else {
        fprintf(stderr, "Error: no class name or --zygote supplied.\n");
        app_usage();
        LOG_ALWAYS_FATAL("app_process: no class name or --zygote supplied.");
        return 10;
    }
}
