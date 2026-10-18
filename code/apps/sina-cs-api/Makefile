CC ?= cc
AR ?= ar
PREFIX ?= /usr/local
BUILD_DIR ?= build
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -fPIC -pthread
CPPFLAGS += -Iinclude
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
.PHONY: all clean check install examples live
all: $(BUILD_DIR)/libsina-cs-api.a $(BUILD_DIR)/libsina-cs-api.so
$(BUILD_DIR):
	mkdir -p $@
$(BUILD_DIR)/%.o: src/%.c src/internal.h $(wildcard include/*) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/libsina-cs-api.a: $(OBJECTS)
	$(AR) rcs $@ $^
$(BUILD_DIR)/libsina-cs-api.so: $(OBJECTS) src/exports.map
	$(CC) -shared -pthread -Wl,-soname,libsina-cs-api.so -Wl,--version-script=src/exports.map -o $@ $(OBJECTS)
install: all
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/cs.h include/cs.sh include/cs_types.h include/cs_kernel.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 $(BUILD_DIR)/libsina-cs-api.a $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(BUILD_DIR)/libsina-cs-api.so $(DESTDIR)$(PREFIX)/lib/
clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/api-smoke: examples/api-smoke.c $(BUILD_DIR)/libsina-cs-api.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libsina-cs-api.a -o $@
examples: $(BUILD_DIR)/api-smoke
$(BUILD_DIR)/test-api: tests/test-api.c tests/mock-nvme.c $(filter-out src/nvme.c,$(SOURCES)) $(wildcard include/*) src/internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(filter %.c,$^) -ldl -o $@

$(BUILD_DIR)/live-api: tests/live-api.c $(BUILD_DIR)/libsina-cs-api.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libsina-cs-api.a -o $@
$(BUILD_DIR)/headers-cxx: tests/headers.cpp $(BUILD_DIR)/libsina-cs-api.a
	$(CXX) $(CPPFLAGS) -std=gnu++17 -Wall -Wextra -Werror $< $(BUILD_DIR)/libsina-cs-api.a -pthread -o $@
check: $(BUILD_DIR)/test-api $(BUILD_DIR)/headers-cxx
	python3 tests/check-api.py
	SINA_CS_FEMU_DEVICES=/dev/nvme0n1:67108864:4 $(BUILD_DIR)/test-api
	$(BUILD_DIR)/headers-cxx
live: $(BUILD_DIR)/live-api
