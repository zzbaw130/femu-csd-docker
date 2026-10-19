CC ?= cc
BUILD_DIR ?= build
SDK_DIR ?= ../snia-cs-api
SDK_BUILD_DIR ?= $(abspath $(SDK_DIR)/build)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pthread
.PHONY: all sdk clean
all: $(BUILD_DIR)/csd-workmem-test
sdk:
	cmake -S $(SDK_DIR) -B $(SDK_BUILD_DIR) -DBUILD_TESTING=OFF
	cmake --build $(SDK_BUILD_DIR) -j2
$(BUILD_DIR)/csd-workmem-test: src/main.c include/workmem-probe.h sdk
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iinclude -I$(SDK_DIR)/include $< -L$(SDK_BUILD_DIR) -lsnia-cs-api -Wl,-rpath,$(SDK_BUILD_DIR) -o $@
clean:
	rm -f $(BUILD_DIR)/csd-workmem-test
