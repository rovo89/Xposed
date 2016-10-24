##########################################################
# Customized app_process executable
##########################################################

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 21)))
  LOCAL_SRC_FILES := app_main2.cpp
  LOCAL_MULTILIB := both
  LOCAL_MODULE_STEM_32 := app_process32_xposed
  LOCAL_MODULE_STEM_64 := app_process64_xposed
else
  LOCAL_SRC_FILES := app_main.cpp
  LOCAL_MODULE_STEM := app_process_xposed
endif

LOCAL_SRC_FILES += \
  xposed.cpp \
  xposed_logcat.cpp \
  xposed_service.cpp \
  xposed_safemode.cpp

LOCAL_SHARED_LIBRARIES := \
  libcutils \
  libutils \
  liblog \
  libbinder \
  libandroid_runtime \
  libdl

LOCAL_CFLAGS += -Wall -Werror -Wextra -Wunused
LOCAL_CFLAGS += -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)

ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 17)))
  LOCAL_SHARED_LIBRARIES += libselinux
  LOCAL_CFLAGS += -DXPOSED_WITH_SELINUX=1
endif

ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 22)))
  LOCAL_WHOLE_STATIC_LIBRARIES := libsigchain
  LOCAL_LDFLAGS := -Wl,--version-script,art/sigchainlib/version-script.txt -Wl,--export-dynamic
endif

ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 23)))
  LOCAL_SHARED_LIBRARIES += libwilhelm
endif

LOCAL_MODULE := xposed
LOCAL_MODULE_TAGS := optional
LOCAL_STRIP_MODULE := keep_symbols

# Always build both architectures (if applicable)
ifeq ($(TARGET_IS_64_BIT),true)
  $(LOCAL_MODULE): $(LOCAL_MODULE)$(TARGET_2ND_ARCH_MODULE_SUFFIX)
endif

include $(BUILD_EXECUTABLE)

##########################################################
# Library for Dalvik-/ART-specific functions
##########################################################
ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 21)))
  include frameworks/base/cmds/xposed/ART.mk
else
  include frameworks/base/cmds/xposed/Dalvik.mk
endif
