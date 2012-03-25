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

LOCAL_C_INCLUDES += dalvik \
                    dalvik/vm \
                    external/stlport/stlport \
                    bionic/ \
                    bionic/libstdc++/include

LOCAL_MODULE:= app_process

include $(BUILD_EXECUTABLE)
