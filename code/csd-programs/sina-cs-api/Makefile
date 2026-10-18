CC ?= cc
BUILD_DIR ?= ../build
OUT := $(abspath $(BUILD_DIR))
SO_CFLAGS ?= -O2 -g -Wall -Wextra -Werror -fPIC
SDK_INCLUDE := ../../apps/sina-cs-api/include
.PHONY: all clean
all: $(OUT)/sina-cs-scale.so
$(OUT):
	mkdir -p $@
$(OUT)/sina-cs-scale.so: scale.c $(SDK_INCLUDE)/cs_kernel.h | $(OUT)
	$(CC) $(SO_CFLAGS) -std=gnu11 -I$(SDK_INCLUDE) -shared -o $@ $<
clean:
	rm -f $(OUT)/sina-cs-scale.so
