#
# Copyright 2007, The Android Open Source Project
# Modified work Copyright (c) 2013, rovo89 and Tungstwenty
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	app_main.cpp \
	dexspy.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	liblog \
	libbinder \
	libandroid_runtime \
	libdvm \
	libstlport

LOCAL_C_INCLUDES += dalvik \
                    dalvik/vm \
                    external/stlport/stlport \
                    bionic \
                    bionic/libstdc++/include

LOCAL_CFLAGS += -DWITH_JIT -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)

LOCAL_MODULE:= app_process

include $(BUILD_EXECUTABLE)


# Build a variant of app_process binary linked with ASan runtime.
# ARM-only at the moment.
ifeq ($(TARGET_ARCH),arm)

include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
	app_main.cpp \
	dexspy.cpp

LOCAL_SHARED_LIBRARIES := \
	libcutils \
	libutils \
	liblog \
	libbinder \
	libandroid_runtime \
	libdvm \
	libstlport

LOCAL_C_INCLUDES += dalvik \
                    dalvik/vm \
                    external/stlport/stlport \
                    bionic \
                    bionic/libstdc++/include

LOCAL_CFLAGS += -DWITH_JIT -DPLATFORM_SDK_VERSION=$(PLATFORM_SDK_VERSION)

LOCAL_MODULE := app_process__asan
LOCAL_MODULE_TAGS := eng
LOCAL_MODULE_PATH := $(TARGET_OUT_EXECUTABLES)/asan
LOCAL_MODULE_STEM := app_process
LOCAL_ADDRESS_SANITIZER := true

include $(BUILD_EXECUTABLE)

endif # ifeq($(TARGET_ARCH),arm)
