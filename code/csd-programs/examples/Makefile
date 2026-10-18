CC ?= cc
CXX ?= c++
include ../mk/csf.mk
CPPFLAGS += $(CSF_CPPFLAGS)
SO_CFLAGS ?= -Wall -Wextra -Werror -O2 -g -fPIC
CXX_SO_CFLAGS ?= -Wall -Wextra -Werror -O2 -g -fPIC
TARGETS := $(OUT)/csd-vadd.so $(OUT)/csd-original-kernels.so
.PHONY: all clean bpf csd-vadd.so csd-original-kernels.so
all: $(TARGETS)
$(OUT):
	mkdir -p $@
$(OUT)/csd-vadd.so: csd-vadd.c femu-csd-kernel.h $(CSF_BUILD_DEPS) | $(OUT)
	$(CC) $(CPPFLAGS) $(SO_CFLAGS) -shared -o $@.tmp $< $(CSF_LDLIBS)
	mv -f $@.tmp $@
$(OUT)/csd-original-kernels.so: csd-original-kernels.cc femu-csd-kernel.h $(CSF_BUILD_DEPS) | $(OUT)
	$(CXX) $(CPPFLAGS) $(CXX_SO_CFLAGS) -shared -o $@.tmp $< -pthread -llz4 $(CSF_LDLIBS)
	mv -f $@.tmp $@
$(OUT)/csd-vadd.bpf.o: csd-vadd.bpf.c femu-csd-kernel.h | $(OUT)
	clang -target bpf -O2 -g -c -o $@ $<
csd-vadd.so: $(OUT)/csd-vadd.so
csd-original-kernels.so: $(OUT)/csd-original-kernels.so
bpf: $(OUT)/csd-vadd.bpf.o
clean:
	rm -f $(TARGETS) $(OUT)/csd-vadd.bpf.o
