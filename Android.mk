LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	app_main.cpp \
	xposed_safemode.cpp \
	xposed.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	libbinder \
	libandroid_runtime \
	libdvm \
	libstlport \
	libdl

ifneq ($(PLATFORM_SDK_VERSION),15)
LOCAL_SHARED_LIBRARIES += libandroidfw
endif

LOCAL_C_INCLUDES += dalvik \
                    dalvik/vm \
                    external/stlport/stlport \
                    bionic \
                    bionic/libstdc++/include

LOCAL_MODULE_TAGS := optional

LOCAL_CFLAGS += -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)

ifeq ($(strip $(WITH_JIT)),true)
LOCAL_CFLAGS += -DWITH_JIT
endif

ifeq ($(strip $(XPOSED_SHOW_OFFSETS)),true)
LOCAL_CFLAGS += -DXPOSED_SHOW_OFFSETS
endif

LOCAL_CFLAGS += -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)
LOCAL_MODULE := xposed
LOCAL_MODULE_STEM := app_process_xposed_sdk$(PLATFORM_SDK_VERSION)

include $(BUILD_EXECUTABLE)
