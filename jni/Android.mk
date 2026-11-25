LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := retro
LOCAL_SRC_FILES := \
	../src/sm8521.c \
	../src/lcd.c \
	../src/audio.c \
	../src/zip_bios.c \
	../src/libretro.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../include $(LOCAL_PATH)/../src
LOCAL_CFLAGS := -std=c99 -O3 -DNDEBUG -Wall -Wextra -Wno-unused-parameter -DANDROID
LOCAL_LDLIBS := -lm -llog

include $(BUILD_SHARED_LIBRARY)
