LOCAL_PATH := $(call my-dir)

ifeq ($(strip $(MAGISK_INCLUDE)),)
$(error MAGISK_INCLUDE must point to the Magisk include directory containing zygisk.hpp)
endif
ifeq ($(wildcard $(MAGISK_INCLUDE)/zygisk.hpp),)
$(error Missing $(MAGISK_INCLUDE)/zygisk.hpp; refusing to build a host-only Zygisk stub)
endif

include $(CLEAR_VARS)
LOCAL_MODULE := yukari
LOCAL_SRC_FILES := src/service_filter.cpp src/native_hooks.cpp src/zygisk_module.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
ifneq ($(MAGISK_INCLUDE),)
LOCAL_C_INCLUDES += $(MAGISK_INCLUDE)
endif
LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra
LOCAL_LDLIBS := -ldl
include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := service-proxy
LOCAL_SRC_FILES := ../service-proxy/service_proxy.cpp src/service_filter.cpp
LOCAL_C_INCLUDES := $(LOCAL_PATH)/include
ifneq ($(MAGISK_INCLUDE),)
LOCAL_C_INCLUDES += $(MAGISK_INCLUDE)
endif
LOCAL_CPPFLAGS := -std=c++17 -Wall -Wextra
include $(BUILD_EXECUTABLE)

