/*
    Certain compile time parameters result in different offsets
    for members in structures. This file defines the offsets for
    members which cannot be accessed otherwise and some macros
    to simplify accessing them.
*/

#define MEMBER_OFFSET_ARRAY(type,member) offsets_array_ ## type ## _ ## member
#define MEMBER_OFFSET_VAR(type,member) offset_ ## type ## _ ## member
#define MEMBER_TYPE(type,member) offset_type_ ## type ## _ ## member

#define MEMBER_PTR(obj,type,member) \
    ( (MEMBER_TYPE(type,member)*)  ( (char*)(obj) + MEMBER_OFFSET_VAR(type,member) ) )
#define MEMBER_VAL(obj,type,member) *MEMBER_PTR(obj,type,member)

#define MEMBER_OFFSET_DEFINE(type,member,offsets...) \
    static int MEMBER_OFFSET_ARRAY(type,member)[] = { offsets }; \
    static int MEMBER_OFFSET_VAR(type,member);
#define MEMBER_OFFSET_COPY(type,member) MEMBER_OFFSET_VAR(type,member) = MEMBER_OFFSET_ARRAY(type,member)[offsetMode]


// here are the definitions of the modes and offsets
enum xposedOffsetModes {
    MEMBER_OFFSET_MODE_WITH_JIT,
    MEMBER_OFFSET_MODE_NO_JIT,
};
static xposedOffsetModes offsetMode;
const char* xposedOffsetModesDesc[] = {
    "WITH_JIT",
    "NO_JIT",
};

MEMBER_OFFSET_DEFINE(Thread, jniLocalRefTable, 168, 100)
#define offset_type_Thread_jniLocalRefTable IndirectRefTable

MEMBER_OFFSET_DEFINE(Thread, status, 856, 120)
#define offset_type_Thread_status ThreadStatus

MEMBER_OFFSET_DEFINE(Thread, jniEnv, 872, 136)
#define offset_type_Thread_jniEnv JNIEnv*

MEMBER_OFFSET_DEFINE(DvmJitGlobals, codeCacheFull, 120, 0)
#define offset_type_DvmJitGlobals_codeCacheFull bool



// helper to determine the required values (compile with XPOSED_SHOW_OFFSET=true)
#ifdef XPOSED_SHOW_OFFSETS
    template<int s> struct RESULT;
    #ifdef WITH_JIT
        #pragma message "WITH_JIT is defined"
    #else
       #pragma message "WITH_JIT is not defined"
    #endif
    RESULT<sizeof(Method)> SIZEOF_Method;
    RESULT<sizeof(Thread)> SIZEOF_Thread;
    RESULT<offsetof(Thread, jniLocalRefTable)> OFFSETOF_Thread_jniLocalRefTable;
    RESULT<offsetof(Thread, status)> OFFSETOF_Thread_status;
    RESULT<offsetof(Thread, jniEnv)> OFFSETOF_Thread_jniEnv;
    RESULT<offsetof(DvmJitGlobals, codeCacheFull)> OFFSETOF_DvmJitGlobals_codeCacheFull;
#endif


