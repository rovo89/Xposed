##########################################################
# Library for Dalvik-specific functions
##########################################################

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
  libxposed_common.cpp \
  libxposed_dalvik.cpp

LOCAL_C_INCLUDES += \
  dalvik \
  dalvik/vm \
  external/stlport/stlport \
  bionic \
  bionic/libstdc++/include \
  libcore/include

LOCAL_SHARED_LIBRARIES := \
  libdvm \
  liblog \
  libdl \
  libnativehelper

ifeq ($(PLATFORM_SDK_VERSION),15)
  LOCAL_SHARED_LIBRARIES += libutils
else
  LOCAL_SHARED_LIBRARIES += libandroidfw
endif

LOCAL_CFLAGS := -Wall -Werror -Wextra -Wunused -Wno-unused-parameter
LOCAL_CFLAGS += -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)

ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 17)))
  LOCAL_CFLAGS += -DXPOSED_WITH_SELINUX=1
endif

LOCAL_MODULE := libxposed_dalvik
LOCAL_MODULE_TAGS := optional
LOCAL_STRIP_MODULE := keep_symbols

include $(BUILD_SHARED_LIBRARY)
