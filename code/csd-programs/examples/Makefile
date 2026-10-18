CC ?= cc
CXX ?= c++
BUILD_DIR ?= ../build
OUT := $(abspath $(BUILD_DIR))
SO_CFLAGS ?= -Wall -Wextra -Werror -O2 -g -fPIC
CXX_SO_CFLAGS ?= -Wall -Wextra -Werror -O2 -g -fPIC
TARGETS := $(OUT)/csd-vadd.so $(OUT)/csd-original-kernels.so
.PHONY: all clean bpf csd-vadd.so csd-original-kernels.so
all: $(TARGETS)
$(OUT):
	mkdir -p $@
$(OUT)/csd-vadd.so: csd-vadd.c femu-csd-kernel.h | $(OUT)
	$(CC) $(SO_CFLAGS) -shared -o $@ $<
$(OUT)/csd-original-kernels.so: csd-original-kernels.cc femu-csd-kernel.h | $(OUT)
	$(CXX) $(CXX_SO_CFLAGS) -shared -o $@ $< -pthread -llz4
$(OUT)/csd-vadd.bpf.o: csd-vadd.bpf.c femu-csd-kernel.h | $(OUT)
	clang -target bpf -O2 -g -c -o $@ $<
csd-vadd.so: $(OUT)/csd-vadd.so
csd-original-kernels.so: $(OUT)/csd-original-kernels.so
bpf: $(OUT)/csd-vadd.bpf.o
clean:
	rm -f $(TARGETS) $(OUT)/csd-vadd.bpf.o
