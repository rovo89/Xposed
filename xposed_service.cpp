/**
 * This file includes the Xposed services, which are especially used to work around SELinux restrictions.
 */

#define LOG_TAG "Xposed"

#include "xposed.h"
#include "xposed_service.h"

#include <binder/BpBinder.h>
#include <binder/IInterface.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/ProcessState.h>
#include <errno.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <sys/mman.h>

#define UID_SYSTEM 1000

using namespace android;

namespace xposed {
namespace service {

////////////////////////////////////////////////////////////
// Declarations
////////////////////////////////////////////////////////////

bool running = false;


////////////////////////////////////////////////////////////
// Memory-based communication (used by Zygote)
////////////////////////////////////////////////////////////

namespace membased {

enum State {
    STATE_NOT_RUNNING,
    STATE_IDLE,
    STATE_SERVICE_ACTION,
    STATE_SERVER_RESPONSE,
};

enum Action {
    OP_NONE,
    OP_ACCESS_FILE,
    OP_STAT_FILE,
    OP_READ_FILE,
};

struct AccessFileData {
    // in
    char path[PATH_MAX];
    int mode;
    // out
    int result;
};

struct StatFileData {
    // in
    char path[PATH_MAX];
    // inout
    struct stat st;
    // out
    int result;
};

struct ReadFileData {
    // in
    char path[PATH_MAX];
    int offset;
    // out
    int totalSize;
    int bytesRead;
    bool eof;
    char content[32*1024];
};

struct MemBasedState {
    pthread_mutex_t workerMutex;
    pthread_cond_t workerCond;
    State state;
    Action action;
    int error;
    union {
        AccessFileData accessFile;
        StatFileData statFile;
        ReadFileData readFile;
    } data;
};

MemBasedState* shared = NULL;
pid_t zygotePid = 0;
bool canAlwaysAccessService = false;

inline static void initSharedMutex(pthread_mutex_t* mutex) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

inline static void initSharedCond(pthread_cond_t* cond) {
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(cond, &cattr);
    pthread_condattr_destroy(&cattr);
}

static bool init() {
    shared = (MemBasedState*) mmap(NULL, sizeof(MemBasedState), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        ALOGE("Could not allocate memory for Zygote service: %s", strerror(errno));
        shared = NULL;
        return false;
    }

    zygotePid = getpid();
    canAlwaysAccessService = true;

    initSharedMutex(&shared->workerMutex);
    initSharedCond(&shared->workerCond);
    shared->state = STATE_NOT_RUNNING;
    shared->action = OP_NONE;
    shared->error = 0;
    return true;
}

void restrictMemoryInheritance() {
    madvise(shared, sizeof(MemBasedState), MADV_DONTFORK);
    canAlwaysAccessService = false;
}

static inline bool isServiceAccessible() {
    if (!canAlwaysAccessService && (shared == NULL || zygotePid != getpid())) {
        ALOGE("Zygote service is not accessible from PID %d, UID %d", getpid(), getuid());
        shared = NULL;
        errno = EPERM;
        return false;
    }
    return true;
}

// Server implementation
void* looper(void* unused __attribute__((unused))) {
    pthread_mutex_lock(&shared->workerMutex);
    shared->state = STATE_IDLE;
    pthread_cond_broadcast(&shared->workerCond);
    while (1) {
        while (shared->state != STATE_SERVICE_ACTION) {
            pthread_cond_wait(&shared->workerCond, &shared->workerMutex);
        }

        switch (shared->action) {
            case OP_ACCESS_FILE: {
                struct AccessFileData* data = &shared->data.accessFile;
                data->result = TEMP_FAILURE_RETRY(access(data->path, data->mode));
                if (data->result != 0) {
                    shared->error = errno;
                }
            } break;

            case OP_STAT_FILE: {
                struct StatFileData* data = &shared->data.statFile;
                data->result = TEMP_FAILURE_RETRY(stat(data->path, &data->st));
                if (data->result != 0) {
                    shared->error = errno;
                }
            } break;

            case OP_READ_FILE: {
                struct ReadFileData* data = &shared->data.readFile;
                struct stat st;

                if (stat(data->path, &st) != 0) {
                    shared->error = errno;
                    break;
                }

                data->totalSize = st.st_size;

                FILE *f = fopen(data->path, "r");
                if (f == NULL) {
                    shared->error = errno;
                    break;
                }

                if (data->offset > 0 && fseek(f, data->offset, SEEK_SET) != 0) {
                    shared->error = ferror(f);
                    fclose(f);
                    break;
                }

                data->bytesRead = fread(data->content, 1, sizeof(data->content), f);
                shared->error = ferror(f);
                data->eof = feof(f);

                fclose(f);
            } break;

            case OP_NONE: {
                ALOGE("No-op call to membased service");
                break;
            }

            default: {
                ALOGE("Invalid action in call to membased service");
                break;
            }
        }

        shared->state = STATE_SERVER_RESPONSE;
        pthread_cond_broadcast(&shared->workerCond);
    }

    pthread_mutex_unlock(&shared->workerMutex);
    return NULL;
}

// Client implementation
static inline bool waitForRunning(int timeout) {
    if (shared == NULL || timeout < 0)
        return false;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 5;
    int rc = 0;
    pthread_mutex_lock(&shared->workerMutex);
    while (shared->state == STATE_NOT_RUNNING && rc == 0) {
        rc = pthread_cond_timedwait(&shared->workerCond, &shared->workerMutex, &ts);
    }
    pthread_mutex_unlock(&shared->workerMutex);
    return rc == 0;
}

static inline void waitForIdle() {
    pthread_mutex_lock(&shared->workerMutex);
    while (shared->state != STATE_IDLE) {
        pthread_cond_wait(&shared->workerCond, &shared->workerMutex);
    }
}

static inline void callService(Action action) {
    shared->action = action;
    shared->state = STATE_SERVICE_ACTION;
    shared->error = 0;
    pthread_cond_broadcast(&shared->workerCond);

    while (shared->state != STATE_SERVER_RESPONSE) {
        pthread_cond_wait(&shared->workerCond, &shared->workerMutex);
    }
}

static inline void makeIdle() {
    shared->action = OP_NONE;
    shared->state = STATE_IDLE;
    pthread_cond_broadcast(&shared->workerCond);
    pthread_mutex_unlock(&shared->workerMutex);
}

int accessFile(const char* path, int mode) {
    if (!isServiceAccessible())
        return -1;

    if (strlen(path) > sizeof(AccessFileData::path) - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }

    waitForIdle();

    struct AccessFileData* data = &shared->data.accessFile;
    strcpy(data->path, path);
    data->mode = mode;

    callService(OP_ACCESS_FILE);

    makeIdle();
    errno = shared->error;
    return shared->error ? -1 : data->result;
}

int statFile(const char* path, struct stat* st) {
    if (!isServiceAccessible())
        return -1;

    if (strlen(path) > sizeof(StatFileData::path) - 1) {
        errno = ENAMETOOLONG;
        return -1;
    }

    waitForIdle();

    struct StatFileData* data = &shared->data.statFile;
    strcpy(data->path, path);

    callService(OP_STAT_FILE);

    memcpy(st, &data->st, sizeof(struct stat));

    makeIdle();
    errno = shared->error;
    return shared->error ? -1 : data->result;
}

char* readFile(const char* path, int* bytesRead) {
    if (!isServiceAccessible())
        return NULL;

    char* result = NULL;
    int offset = 0, totalSize = 0;

    if (bytesRead)
        *bytesRead = 0;

    if (strlen(path) > sizeof(ReadFileData::path) - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    waitForIdle();

    struct ReadFileData* data = &shared->data.readFile;
    strcpy(data->path, path);
    data->offset = 0;

    callService(OP_READ_FILE);
    if (shared->error)
        goto bail;

    totalSize = data->totalSize;
    result = (char*) malloc(totalSize + 1);
    result[totalSize] = 0;
    memcpy(result, data->content, data->bytesRead);

    while (!data->eof) {
        offset += data->bytesRead;
        data->offset = offset;

        callService(OP_READ_FILE);
        if (shared->error)
            goto bail;

        if (offset + data->bytesRead > totalSize) {
            shared->error = EBUSY;
            goto bail;
        }

        memcpy(result + offset, data->content, data->bytesRead);
    }

    if (bytesRead)
        *bytesRead = offset + data->bytesRead;

    bail:
    makeIdle();
    if (shared->error && result) {
        free(result);
        result = NULL;
    }
    errno = shared->error;
    return result;
}

}  // namespace membased


////////////////////////////////////////////////////////////
// Binder service
////////////////////////////////////////////////////////////

namespace binder {

#define XPOSED_BINDER_SYSTEM_SERVICE_NAME "user.xposed.system"
#define XPOSED_BINDER_APP_SERVICE_NAME    "user.xposed.app"

class IXposedService: public IInterface {
    public:
        DECLARE_META_INTERFACE(XposedService);
        virtual int test() const = 0;
        virtual status_t addService(const String16& name,
                                    const sp<IBinder>& service,
                                    bool allowIsolated = false) const = 0;
        virtual int accessFile(const String16& filename,
                               int32_t mode) const = 0;
        virtual int statFile(const String16& filename,
                             int64_t* size,
                             int64_t* mtime) const = 0;
        virtual status_t readFile(const String16& filename,
                                  int32_t offset,
                                  int32_t length,
                                  int64_t* size,
                                  int64_t* mtime,
                                  uint8_t** buffer,
                                  int32_t* bytesRead,
                                  String16* errormsg) const = 0;

        enum {
            TEST_TRANSACTION = IBinder::FIRST_CALL_TRANSACTION,
            ADD_SERVICE_TRANSACTION,
            ACCESS_FILE_TRANSACTION,
            STAT_FILE_TRANSACTION,
            READ_FILE_TRANSACTION,
        };
};

class BnXposedService: public BnInterface<IXposedService> {
    public:
        virtual status_t onTransact( uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags = 0);
};

class BpXposedService: public BpInterface<IXposedService> {
    public:
        BpXposedService(const sp<IBinder>& impl) : BpInterface<IXposedService>(impl) {}

        virtual int test() const {
            Parcel data, reply;
            data.writeInterfaceToken(IXposedService::getInterfaceDescriptor());
            remote()->transact(TEST_TRANSACTION, data, &reply);
            if (reply.readExceptionCode() != 0) return -1;
            return reply.readInt32();
        }

        virtual status_t addService(const String16& name, const sp<IBinder>& service,
                bool allowIsolated = false) const {
            Parcel data, reply;
            data.writeInterfaceToken(IXposedService::getInterfaceDescriptor());
            data.writeString16(name);
            data.writeStrongBinder(service);
            data.writeInt32(allowIsolated ? 1 : 0);
            status_t err = remote()->transact(ADD_SERVICE_TRANSACTION, data, &reply);
            return err == NO_ERROR ? reply.readExceptionCode() : err;
        }

        virtual status_t accessFile(const String16& name, int32_t mode) const {
            Parcel data, reply;
            data.writeInterfaceToken(IXposedService::getInterfaceDescriptor());
            data.writeString16(name);
            data.writeInt32(mode);

            remote()->transact(ACCESS_FILE_TRANSACTION, data, &reply);
            if (reply.readExceptionCode() != 0) return -1;

            errno = reply.readInt32();
            return (errno == 0) ? 0 : -1;
        }

        virtual status_t statFile(const String16& name, int64_t* size, int64_t* mtime) const {
            Parcel data, reply;
            data.writeInterfaceToken(IXposedService::getInterfaceDescriptor());
            data.writeString16(name);

            remote()->transact(STAT_FILE_TRANSACTION, data, &reply);
            if (reply.readExceptionCode() != 0) return -1;

            errno = reply.readInt32();
            if (errno != 0) return -1;

            int64_t size1 = reply.readInt64();
            int64_t mtime1 = reply.readInt64();
            if (size != NULL) *size = size1;
            if (mtime != NULL) *mtime = mtime1;
            return 0;
        }

        virtual status_t readFile(const String16& filename, int32_t offset, int32_t length,
                int64_t* size, int64_t* mtime, uint8_t** buffer, int32_t* bytesRead, String16* errormsg) const {
            Parcel data, reply;
            data.writeInterfaceToken(IXposedService::getInterfaceDescriptor());
            data.writeString16(filename);
            data.writeInt32(offset);
            data.writeInt32(length);
            int64_t size1 = 0;
            int64_t mtime1 = 0;
            if (size != NULL) size1 = *size;
            if (mtime != NULL) mtime1 = *mtime;
            data.writeInt64(size1);
            data.writeInt64(mtime1);

            remote()->transact(READ_FILE_TRANSACTION, data, &reply);
            if (reply.readExceptionCode() != 0) return -1;

            status_t err = reply.readInt32();
            const String16& errormsg1(reply.readString16());
            size1 = reply.readInt64();
            mtime1 = reply.readInt64();
            int32_t bytesRead1 = reply.readInt32();
            if (size != NULL) *size = size1;
            if (mtime != NULL) *mtime = mtime1;
            if (bytesRead != NULL) *bytesRead = bytesRead1;
            if (errormsg) *errormsg = errormsg1;

            if (bytesRead1 > 0 && bytesRead1 <= (int32_t)reply.dataAvail()) {
                *buffer = (uint8_t*) malloc(bytesRead1 + 1);
                *buffer[bytesRead1] = 0;
                reply.read(*buffer, bytesRead1);
            } else {
                *buffer = NULL;
            }

            errno = err;
            return (errno == 0) ? 0 : -1;
        }
};

IMPLEMENT_META_INTERFACE(XposedService, "de.robv.android.xposed.IXposedService");

status_t BnXposedService::onTransact(uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)  {
    switch (code) {
        case TEST_TRANSACTION: {
            CHECK_INTERFACE(IXposedService, data, reply);
            reply->writeNoException();
            reply->writeInt32(test());
            return NO_ERROR;
        } break;

        case ADD_SERVICE_TRANSACTION: {
            CHECK_INTERFACE(IXposedService, data, reply);
            String16 which = data.readString16();
            sp<IBinder> b = data.readStrongBinder();
            bool allowIsolated = (data.readInt32() != 0);
            reply->writeInt32(addService(which, b, allowIsolated));
            return NO_ERROR;
        } break;

        case ACCESS_FILE_TRANSACTION: {
            CHECK_INTERFACE(IXposedService, data, reply);
            String16 filename = data.readString16();
            int32_t mode = data.readInt32();
            status_t result = accessFile(filename, mode);
            int err = errno;
            reply->writeNoException();
            reply->writeInt32(result == 0 ? 0 : err);
            return NO_ERROR;
        } break;

        case STAT_FILE_TRANSACTION: {
            CHECK_INTERFACE(IXposedService, data, reply);
            String16 filename = data.readString16();
            int64_t size, time;
            status_t result = statFile(filename, &size, &time);
            int err = errno;
            reply->writeNoException();
            if (result == 0) {
                reply->writeInt32(0);
                reply->writeInt64(size);
                reply->writeInt64(time);
            } else {
                reply->writeInt32(err);
            }
            return NO_ERROR;
        } break;

        case READ_FILE_TRANSACTION: {
            CHECK_INTERFACE(IXposedService, data, reply);
            String16 filename = data.readString16();
            int32_t offset = data.readInt32();
            int32_t length = data.readInt32();
            int64_t size = data.readInt64();
            int64_t mtime = data.readInt64();
            uint8_t* buffer = NULL;
            int32_t bytesRead = -1;
            String16 errormsg;

            status_t err = readFile(filename, offset, length, &size, &mtime, &buffer, &bytesRead, &errormsg);

            reply->writeNoException();
            reply->writeInt32(err);
            reply->writeString16(errormsg);
            reply->writeInt64(size);
            reply->writeInt64(mtime);
            if (bytesRead > 0) {
                reply->writeInt32(bytesRead);
                reply->write(buffer, bytesRead);
                free(buffer);
            } else {
                reply->writeInt32(bytesRead); // empty array (0) or null (-1)
            }
            return NO_ERROR;
        } break;

        default:
            return BBinder::onTransact(code, data, reply, flags);
    }
}

class XposedService : public BnXposedService {
    public:
        XposedService(bool system);

        virtual int test() const;
        virtual status_t addService(const String16& name,
                                    const sp<IBinder>& service,
                                    bool allowIsolated = false) const;
        virtual status_t accessFile(const String16& filename16,
                                    int32_t mode) const;
        virtual status_t statFile(const String16& filename,
                                  int64_t* size,
                                  int64_t* mtime) const;
        virtual status_t readFile(const String16& filename16,
                                  int32_t offset,
                                  int32_t length,
                                  int64_t* size,
                                  int64_t* mtime,
                                  uint8_t** buffer,
                                  int32_t* bytesRead,
                                  String16* errormsg) const;

    private:
        bool isSystem;
};

static String16 formatToString16(const char* fmt, ...) {
    char* message;
    va_list args;
    va_start(args, fmt);
    int size = vasprintf(&message, fmt, args);
    String16 result(message, size);
    free(message);
    va_end(args);
    return result;
}

XposedService::XposedService(bool system)
        : isSystem(system) {}

int XposedService::test() const {
    pid_t pid = IPCThreadState::self()->getCallingPid();
    ALOGD("This is PID %d, test method was called from PID %d", getpid(), pid);
    return getpid();
}

status_t XposedService::addService(const String16& name, const sp<IBinder>& service,
        bool allowIsolated) const {
    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (!isSystem || (uid != xposed->installer_uid)) {
        ALOGE("Permission denied, not adding service %s", String8(name).string());
        errno = EPERM;
        return -1;
    }
    sp<IServiceManager> sm = defaultServiceManager();
#if PLATFORM_SDK_VERSION >= 16
    return sm->addService(name, service, allowIsolated);
#else
    return sm->addService(name, service);
#endif
}

status_t XposedService::accessFile(const String16& filename16, int32_t mode) const {
    uid_t caller = IPCThreadState::self()->getCallingUid();
    if (caller != UID_SYSTEM) {
        ALOGE("UID %d is not allowed to use the Xposed service", caller);
        errno = EPERM;
        return -1;
    }
    const char* filename = String8(filename16).string();
    return TEMP_FAILURE_RETRY(access(filename, mode));
}

status_t XposedService::statFile(const String16& filename16, int64_t* size, int64_t* time) const {
    uid_t caller = IPCThreadState::self()->getCallingUid();
    if (caller != UID_SYSTEM) {
        ALOGE("UID %d is not allowed to use the Xposed service", caller);
        errno = EPERM;
        return -1;
    }
    const char* filename = String8(filename16).string();
    struct stat st;
    status_t result = TEMP_FAILURE_RETRY(stat(filename, &st));
    if (result == 0) {
        *size = st.st_size;
        *time = st.st_mtime;
    }
    return result;
}

status_t XposedService::readFile(const String16& filename16, int32_t offset, int32_t length,
        int64_t* size, int64_t* mtime, uint8_t** buffer, int32_t* bytesRead, String16* errormsg) const {

    uid_t caller = IPCThreadState::self()->getCallingUid();
    if (caller != UID_SYSTEM) {
        ALOGE("UID %d is not allowed to use the Xposed service", caller);
        return EPERM;
    }

    *buffer = NULL;
    *bytesRead = -1;

    // Get file metadata
    const char* filename = String8(filename16).string();
    struct stat st;
    if (stat(filename, &st) != 0) {
        status_t err = errno;
        if (errormsg) *errormsg = formatToString16("%s during stat() on %s", strerror(err), filename);
        return err;
    }

    if (S_ISDIR(st.st_mode)) {
        if (errormsg) *errormsg = formatToString16("%s is a directory", filename);
        return EISDIR;
    }

    // Don't load again if file is unchanged
    if (*size == st.st_size && *mtime == (int32_t)st.st_mtime) {
        return 0;
    }

    *size = st.st_size;
    *mtime = st.st_mtime;

    // Check range
    if (offset > 0 && offset >= *size) {
        if (errormsg) *errormsg = formatToString16("offset %d >= size %" PRId64 " for %s", offset, *size, filename);
        return EINVAL;
    } else if (offset < 0) {
        offset = 0;
    }

    if (length > 0 && (offset + length) > *size) {
        if (errormsg) *errormsg = formatToString16("offset %d + length %d > size %" PRId64 " for %s", offset, length, *size, filename);
        return EINVAL;
    } else if (*size == 0) {
        *bytesRead = 0;
        return 0;
    } else if (length <= 0) {
        length = *size - offset;
    }

    // Allocate buffer
    *buffer = (uint8_t*) malloc(length + 1);
    if (*buffer == NULL) {
        if (errormsg) *errormsg = formatToString16("allocating buffer with %d bytes failed", length + 1);
        return ENOMEM;
    }
    (*buffer)[length] = 0;

    // Open file
    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        status_t err = errno;
        free(*buffer);
        *buffer = NULL;
        if (errormsg) *errormsg = formatToString16("%s during fopen() on %s", strerror(err), filename);
        return err;
    }

    // Seek to correct offset
    if (offset > 0 && fseek(f, offset, SEEK_SET) != 0) {
        free(*buffer);
        *buffer = NULL;
        status_t err = ferror(f);
        fclose(f);
        if (errormsg) *errormsg = formatToString16("%s during fseek() to offset %d for %s", strerror(err), offset, filename);
        return err;
    }

    // Read the file
    *bytesRead = fread(*buffer, 1, length, f);
    status_t err = ferror(f);
    if (err != 0) {
        free(*buffer);
        *buffer = NULL;
        *bytesRead = -1;
        if (errormsg) *errormsg = formatToString16("%s during fread(), read %d bytes for %s", strerror(err), *bytesRead, filename);
    }

    // Close the file
    fclose(f);

    return err;
}

}  // namespace binder


////////////////////////////////////////////////////////////
// General
////////////////////////////////////////////////////////////

static void systemService() {
    xposed::setProcessName("xposed_service_system");
    xposed::dropCapabilities();

#if XPOSED_WITH_SELINUX
    if (xposed->isSELinuxEnabled) {
        if (setcon(ctx_system) != 0) {
            ALOGE("Could not switch to %s context", ctx_system);
            exit(EXIT_FAILURE);
        }
    }
#endif  // XPOSED_WITH_SELINUX

    // Initialize the system service
    sp<IServiceManager> sm(defaultServiceManager());
#if PLATFORM_SDK_VERSION >= 16
    status_t err = sm->addService(String16(XPOSED_BINDER_SYSTEM_SERVICE_NAME), new binder::XposedService(true), true);
#else
    status_t err = sm->addService(String16(XPOSED_BINDER_SYSTEM_SERVICE_NAME), new binder::XposedService(true));
#endif
    if (err != NO_ERROR) {
        ALOGE("Error %d while adding system service %s", err, XPOSED_BINDER_SYSTEM_SERVICE_NAME);
        exit(EXIT_FAILURE);
    }

    sp<ProcessState> ps(ProcessState::self());
    ps->startThreadPool();
#if PLATFORM_SDK_VERSION >= 18
    ps->giveThreadPoolName();
#endif
    IPCThreadState::self()->joinThreadPool();
}

static void appService() {
    xposed::setProcessName("xposed_service_app");
    if (!xposed::switchToXposedInstallerUidGid()) {
        exit(EXIT_FAILURE);
    }
    xposed::dropCapabilities();

#if XPOSED_WITH_SELINUX
    if (xposed->isSELinuxEnabled) {
        if (setcon(ctx_app) != 0) {
            ALOGE("Could not switch to %s context", ctx_app);
            exit(EXIT_FAILURE);
        }
    }
#endif  // XPOSED_WITH_SELINUX

    // We have to register the app service by using the already running system service as a proxy
    sp<IServiceManager> sm(defaultServiceManager());
    sp<IBinder> systemBinder = sm->getService(String16(XPOSED_BINDER_SYSTEM_SERVICE_NAME));
    sp<binder::IXposedService> xposedSystemService = interface_cast<binder::IXposedService>(systemBinder);
    status_t err = xposedSystemService->addService(String16(XPOSED_BINDER_APP_SERVICE_NAME), new binder::XposedService(false), true);

    // Check result for the app service registration
    if (err != NO_ERROR) {
        ALOGE("Error %d while adding app service %s", err, XPOSED_BINDER_APP_SERVICE_NAME);
        exit(EXIT_FAILURE);
    }

#if XPOSED_WITH_SELINUX
    // Initialize the memory-based Zygote service
    if (xposed->isSELinuxEnabled) {
        pthread_t thMemBased;
        if (pthread_create(&thMemBased, NULL, &membased::looper, NULL) != 0) {
            ALOGE("Could not create thread for memory-based service: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
#endif  // XPOSED_WITH_SELINUX

    sp<ProcessState> ps(ProcessState::self());
    ps->startThreadPool();
#if PLATFORM_SDK_VERSION >= 18
    ps->giveThreadPoolName();
#endif
    IPCThreadState::self()->joinThreadPool();
}

bool checkMembasedRunning() {
    // Ensure that the memory based service is running
    if (!membased::waitForRunning(5)) {
        ALOGE("Xposed's Zygote service is not running, cannot work without it");
        return false;
    }

    return true;
}

bool startAll() {
    if (xposed->isSELinuxEnabled && !membased::init()) {
        return false;
    }

    // system context service
    pid_t pid;
    if ((pid = fork()) < 0) {
        ALOGE("Fork for Xposed service in system context failed: %s", strerror(errno));
        return false;
    } else if (pid == 0) {
        systemService();
        // Should never reach this point
        exit(EXIT_FAILURE);
    }

    // app context service
    if ((pid = fork()) < 0) {
        ALOGE("Fork for Xposed service in app context failed: %s", strerror(errno));
        return false;
    } else if (pid == 0) {
        appService();
        // Should never reach this point
        exit(EXIT_FAILURE);
    }

    if (xposed->isSELinuxEnabled && !checkMembasedRunning()) {
        return false;
    }

    return true;
}

#if XPOSED_WITH_SELINUX
bool startMembased() {
    if (!xposed->isSELinuxEnabled) {
        return true;
    }

    if (!membased::init()) {
        return false;
    }

    pid_t pid;
    if ((pid = fork()) < 0) {
        ALOGE("Fork for Xposed Zygote service failed: %s", strerror(errno));
        return false;
    } else if (pid == 0) {
        xposed::setProcessName("xposed_zygote_service");
        if (!xposed::switchToXposedInstallerUidGid()) {
            exit(EXIT_FAILURE);
        }
        xposed::dropCapabilities();
        if (setcon(ctx_app) != 0) {
            ALOGE("Could not switch to %s context", ctx_app);
            exit(EXIT_FAILURE);
        }
        membased::looper(NULL);
        // Should never reach this point
        exit(EXIT_FAILURE);
    }

    return checkMembasedRunning();
}
#endif  // XPOSED_WITH_SELINUX

}  // namespace service
}  // namespace xposed
