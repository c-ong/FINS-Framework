LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := fins_tcp
#LOCAL_SRC_FILES := tcp.c tcp_in.c tcp_out.c
LS_C=$(subst $(1)/,,$(wildcard $(1)/*.c))
LOCAL_SRC_FILES := $(call LS_C,$(LOCAL_PATH))
LOCAL_STATIC_LIBRARIES := fins_common fins_switch
LOCAL_CFLAGS := -DBUILD_FOR_ANDROID -g -O2 -Wall
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)
include $(BUILD_STATIC_LIBRARY)

$(call import-module,common)
$(call import-module,core/switch)