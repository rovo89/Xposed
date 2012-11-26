LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	app_main.cpp \
	xposed.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	libbinder \
	libandroid_runtime \
	libdvm \
	libstlport

LOCAL_STATIC_LIBRARIES += libbz

LOCAL_C_INCLUDES += dalvik \
                    dalvik/vm \
                    external/stlport/stlport \
                    external/bzip2 \
                    bionic \
                    bionic/libstdc++/include

LOCAL_MODULE_TAGS := optional

LOCAL_CFLAGS += -DWITH_JIT

# use "mm xposed_target=ics" to compile this in pre-JB source tree for ICS
ifeq ($(xposed_target),ics)
LOCAL_MODULE := xposed_ics
LOCAL_CFLAGS += -DXPOSED_TARGET_ICS
else
LOCAL_SHARED_LIBRARIES += libandroidfw
LOCAL_MODULE := xposed
endif

include $(BUILD_EXECUTABLE)

### binary compatibility test executable
include $(CLEAR_VARS)
LOCAL_SRC_FILES := xposedtest.cpp
LOCAL_MODULE := xposedtest
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -O0 # no optimizations like inline etc.
include $(BUILD_EXECUTABLE)

