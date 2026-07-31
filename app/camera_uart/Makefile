CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

APP_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SDK_ROOT := $(abspath $(APP_DIR)/../..)
CROSS_CC := $(SDK_ROOT)/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc

.PHONY: all aarch64 clean

all: camera_uart_test

camera_uart_test: camera_uart_test.c
	$(CC) $(CFLAGS) $< -o $@

aarch64:
	$(MAKE) -f $(lastword $(MAKEFILE_LIST)) clean
	$(MAKE) -f $(lastword $(MAKEFILE_LIST)) CC=$(CROSS_CC) all

clean:
	$(RM) camera_uart_test
