CC ?= cc
CXX ?= c++
CSD_WORKMEM_IMPLEMENTATION := 1
include ../mk/csf.mk
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -fPIC -pthread
CPPFLAGS += -Iinclude
SDK_INCLUDE := ../../apps/sina-cs-api/include
PROBE_INCLUDE := ../../apps/csd-workmem-test/include
LIB := $(OUT)/libcs-workmem.so
PROBES := $(OUT)/csd-workmem-probe-a.so $(OUT)/csd-workmem-probe-b.so
.PHONY: all library check clean
all: $(LIB) $(PROBES)
library: $(LIB)
$(OUT):
	mkdir -p $@
$(LIB): src/workmem.c include/cs_workmem.h src/exports.map Makefile | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -shared -Wl,-soname,libcs-workmem.so \
	    -Wl,--version-script=src/exports.map $< -o $@.tmp
	mv -f $@.tmp $@
$(OUT)/csd-workmem-probe-a.so: examples/probe.c include/cs_workmem.h $(SDK_INCLUDE)/femu-csd-kernel.h $(PROBE_INCLUDE)/workmem-probe.h $(CSF_BUILD_DEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(SDK_INCLUDE) -I$(PROBE_INCLUDE) -DPROBE_VARIANT=1 \
	    -shared $< $(CSF_LDLIBS) -o $@.tmp
	mv -f $@.tmp $@
$(OUT)/csd-workmem-probe-b.so: examples/probe.c include/cs_workmem.h $(SDK_INCLUDE)/femu-csd-kernel.h $(PROBE_INCLUDE)/workmem-probe.h $(CSF_BUILD_DEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -I$(SDK_INCLUDE) -I$(PROBE_INCLUDE) -DPROBE_VARIANT=2 \
	    -shared $< $(CSF_LDLIBS) -o $@.tmp
	mv -f $@.tmp $@
$(OUT)/test-workmem: tests/test-workmem.c $(LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -L$(OUT) -lcs-workmem -Wl,-rpath,'$$ORIGIN' -o $@
$(OUT)/test-workmem-shared: tests/test-shared.c $(PROBES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -ldl -o $@
$(OUT)/test-workmem-cxx: tests/headers.cpp $(LIB)
	$(CXX) $(CPPFLAGS) -std=c++17 -Wall -Wextra -Werror $< \
	    -L$(OUT) -lcs-workmem -Wl,-rpath,'$$ORIGIN' -o $@
check: all $(OUT)/test-workmem $(OUT)/test-workmem-shared $(OUT)/test-workmem-cxx
	$(OUT)/test-workmem
	$(OUT)/test-workmem-shared $(OUT)/csd-workmem-probe-a.so $(OUT)/csd-workmem-probe-b.so
	$(OUT)/test-workmem-cxx
clean:
	rm -f $(LIB) $(PROBES) $(OUT)/test-workmem $(OUT)/test-workmem-shared $(OUT)/test-workmem-cxx
