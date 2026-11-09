# Pod Looper firmware for the Electrosmith Daisy Pod.
#
#   git submodule update --init --recursive   (first time only)
#   make -C libDaisy                          (first time only)
#   make                                      -> build/pod-looper.bin
#   make program-dfu                          (Pod in DFU mode: hold BOOT, tap RESET)

TARGET = pod-looper

CPP_SOURCES = src/main.cpp src/looper.cpp src/synth.cpp

LIBDAISY_DIR = libDaisy

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
