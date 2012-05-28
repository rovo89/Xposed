LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
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

LOCAL_MODULE:= xposed
LOCAL_MODULE_TAGS := optional

LOCAL_CFLAGS += -DWITH_JIT

include $(BUILD_EXECUTABLE)
