##########################################################
# Library for ART-specific functions
##########################################################

include $(CLEAR_VARS)

include art/build/Android.common_build.mk
$(eval $(call set-target-local-clang-vars))
$(eval $(call set-target-local-cflags-vars,ndebug))

ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 23)))
  LOCAL_C_INCLUDES += \
    external/valgrind \
    external/valgrind/include
else
  include external/libcxx/libcxx.mk
  LOCAL_C_INCLUDES += \
    external/valgrind/main \
    external/valgrind/main/include
endif

LOCAL_SRC_FILES += \
  libxposed_common.cpp \
  libxposed_art.cpp

LOCAL_C_INCLUDES += \
  art/runtime \
  external/gtest/include

ifeq (1,$(strip $(shell expr $(PLATFORM_SDK_VERSION) \>= 24)))
  LOCAL_C_INCLUDES += bionic/libc/private
endif

LOCAL_SHARED_LIBRARIES += \
  libart \
  liblog \
  libcutils \
  libandroidfw \
  libnativehelper

LOCAL_CFLAGS += \
  -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION) \
  -DXPOSED_WITH_SELINUX=1

LOCAL_MODULE := libxposed_art
LOCAL_MODULE_TAGS := optional
LOCAL_STRIP_MODULE := keep_symbols
LOCAL_MULTILIB := both

# Always build both architectures (if applicable)
ifeq ($(TARGET_IS_64_BIT),true)
  $(LOCAL_MODULE): $(LOCAL_MODULE)$(TARGET_2ND_ARCH_MODULE_SUFFIX)
endif

include $(BUILD_SHARED_LIBRARY)
