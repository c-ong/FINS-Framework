LOCAL_DIR := $(PWD)

ifndef FINS_ROOT_DIR
export FINS_ROOT_DIR := $(abspath $(LOCAL_DIR)/../../..)
endif

# fins.mk contains the compiler and linker options for each target platform
include $(FINS_ROOT_DIR)/settings.finsmk

#Name of the module
LOCAL_NAME := ipv4
#LOCAL_OBJS := IP4_checksum.o IP4_const_header.o IP4_dest_check.o IP4_exit.o IP4_forward.o IP4_fragment_data.o IP4_in.o IP4_init.o IP4_next_hop.o IP4_out.o IP4_reass.o IP4_receive_fdf.o IP4_route_info.o IP4_send_fdf.o ipv4.o
LOCAL_OBJS := ipv4.o ipv4_checksum.o ipv4_const_header.o ipv4_forward.o ipv4_in.o ipv4_out.o ipv4_send.o ipv4_receive.o
LOCAL_INCS := .
LOCAL_CFLAGS := -fpic

ifdef BUILD_FOR_ANDROID_ARM
LOCAL_LIBS := config common data_structure
LOCAL_LDFLAGS := -lc
else
LOCAL_LIBS := data_structure 
LOCAL_LDFLAGS := -lc -lpthread
endif

include $(MODULES_DIR)/make_module.finsmk