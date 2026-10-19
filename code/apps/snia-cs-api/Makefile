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
all: $(BUILD_DIR)/libsnia-cs-api.a $(BUILD_DIR)/libsnia-cs-api.so
$(BUILD_DIR):
	mkdir -p $@
$(BUILD_DIR)/%.o: src/%.c src/internal.h $(wildcard include/*) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/libsnia-cs-api.a: $(OBJECTS)
	$(AR) rcs $@ $^
$(BUILD_DIR)/libsnia-cs-api.so: $(OBJECTS) src/exports.map
	$(CC) -shared -pthread -Wl,-soname,libsnia-cs-api.so -Wl,--version-script=src/exports.map -o $@ $(OBJECTS)
install: all
	install -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	install -m 644 include/cs.h include/cs.sh include/cs_types.h include/femu-csd-kernel.h include/cs_csf.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 $(BUILD_DIR)/libsnia-cs-api.a $(DESTDIR)$(PREFIX)/lib/
	install -m 755 $(BUILD_DIR)/libsnia-cs-api.so $(DESTDIR)$(PREFIX)/lib/
clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/api-smoke: examples/api-smoke.c $(BUILD_DIR)/libsnia-cs-api.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libsnia-cs-api.a -o $@
examples: $(BUILD_DIR)/api-smoke
$(BUILD_DIR)/test-api: tests/test-api.c tests/mock-nvme.c tests/spill-scenario.h $(filter-out src/nvme.c,$(SOURCES)) $(wildcard include/*) src/internal.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(filter %.c,$^) -ldl -o $@

$(BUILD_DIR)/live-api: tests/live-api.c tests/spill-scenario.h $(BUILD_DIR)/libsnia-cs-api.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libsnia-cs-api.a -o $@
$(BUILD_DIR)/headers-cxx: tests/headers.cpp $(BUILD_DIR)/libsnia-cs-api.a
	$(CXX) $(CPPFLAGS) -std=gnu++17 -Wall -Wextra -Werror $< $(BUILD_DIR)/libsnia-cs-api.a -pthread -o $@
$(BUILD_DIR)/test-identify: tests/test-identify.c $(BUILD_DIR)/libsnia-cs-api.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD_DIR)/libsnia-cs-api.a \
	    -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=glob,--wrap=globfree -o $@
check: $(BUILD_DIR)/test-api $(BUILD_DIR)/headers-cxx $(BUILD_DIR)/test-identify
	python3 tests/check-api.py
	$(BUILD_DIR)/test-api
	$(BUILD_DIR)/headers-cxx
	$(BUILD_DIR)/test-identify
live: $(BUILD_DIR)/live-api
