CC ?= cc
BUILD_DIR ?= build
SDK_DIR ?= ../sina-cs-api
SDK_BUILD_DIR ?= $(abspath $(SDK_DIR)/build)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pthread
.PHONY: all sdk clean
all: $(BUILD_DIR)/csd-workmem-test
sdk:
	$(MAKE) -C $(SDK_DIR) BUILD_DIR=$(SDK_BUILD_DIR) all
$(BUILD_DIR)/csd-workmem-test: src/main.c include/workmem-probe.h | sdk
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iinclude -I$(SDK_DIR)/include $< $(SDK_BUILD_DIR)/libsina-cs-api.a -o $@
clean:
	rm -f $(BUILD_DIR)/csd-workmem-test
