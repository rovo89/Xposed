##########################################################
# Library for ART-specific functions
##########################################################

include $(CLEAR_VARS)

ifeq ($(PLATFORM_SDK_VERSION),19)
    include art/build/Android.common.mk
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    LOCAL_CFLAGS := $(ART_TARGET_CFLAGS) $(ART_TARGET_NON_DEBUG_CFLAGS)

    include external/stlport/libstlport.mk
else
    include art/build/Android.common_build.mk
    $(eval $(call set-target-local-clang-vars))
    $(eval $(call set-target-local-cflags-vars,ndebug))

    include external/libcxx/libcxx.mk
    LOCAL_C_INCLUDES += \
        external/valgrind/main \
        external/valgrind/main/include
    LOCAL_MULTILIB := both
endif

LOCAL_SRC_FILES += \
    libxposed_common.cpp \
    libxposed_art.cpp

LOCAL_C_INCLUDES += \
    art/runtime \
    external/gtest/include

LOCAL_SHARED_LIBRARIES += \
    libart \
    liblog \
    libcutils \
    libandroidfw \
    libnativehelper

LOCAL_CFLAGS += \
    -Wno-unused-parameter \
    -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION) \
    -DXPOSED_WITH_SELINUX=1

LOCAL_MODULE := libxposed_art
LOCAL_MODULE_TAGS := optional
LOCAL_STRIP_MODULE := keep_symbols

# Always build both architecture (if applicable)
ifeq ($(TARGET_IS_64_BIT),true)
    $(LOCAL_MODULE): $(LOCAL_MODULE)$(TARGET_2ND_ARCH_MODULE_SUFFIX)
endif

include $(BUILD_SHARED_LIBRARY)
