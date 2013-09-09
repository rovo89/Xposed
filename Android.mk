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
	libstlport \
	libdl

ifneq ($(shell expr $(PLATFORM_SDK_VERSION) \> 15), 0)
LOCAL_SHARED_LIBRARIES += libandroidfw
endif

LOCAL_C_INCLUDES += dalvik \
                    dalvik/vm \
                    external/stlport/stlport \
                    bionic \
                    bionic/libstdc++/include

LOCAL_MODULE_TAGS := optional
ifneq ($(TARGET_ARCH),x86)
LOCAL_CFLAGS += -DWITH_JIT
endif
LOCAL_CFLAGS += -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)
LOCAL_MODULE := xposed
LOCAL_MODULE_STEM := app_process_xposed_sdk$(PLATFORM_SDK_VERSION)

include $(BUILD_EXECUTABLE)

### binary compatibility test executable
include $(CLEAR_VARS)
LOCAL_SRC_FILES := xposedtest.cpp
LOCAL_MODULE := xposedtest
LOCAL_MODULE_STEM := xposedtest_sdk$(PLATFORM_SDK_VERSION)
LOCAL_MODULE_TAGS := optional
LOCAL_CFLAGS += -O0 # no optimizations like inline etc.
include $(BUILD_EXECUTABLE)

