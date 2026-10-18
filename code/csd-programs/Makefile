CC ?= gcc
CXX ?= c++
CFLAGS ?= -Wall -Wextra -O2 -g
SO_CFLAGS ?= -Wall -Wextra -O2 -g -fPIC
CXX_SO_CFLAGS ?= -Wall -Wextra -O2 -g -fPIC

BPF_TARGETS := csd-vadd.bpf.o
TARGETS := csd-vadd.so csd-original-kernels.so

.PHONY: all bpf clean

all: $(TARGETS)

bpf: $(BPF_TARGETS)

csd-vadd.so: csd-vadd.c femu-csd-kernel.h
	$(CC) $(SO_CFLAGS) -shared -o $@ $<

csd-original-kernels.so: csd-original-kernels.cc femu-csd-kernel.h
	$(CXX) $(CXX_SO_CFLAGS) -shared -o $@ $< -pthread -llz4

csd-vadd.bpf.o: csd-vadd.bpf.c femu-csd-kernel.h
	clang -target bpf -O2 -g -c -o $@ $<

clean:
	rm -f $(TARGETS) $(BPF_TARGETS)

# SNIA API examples: run on FEMU CU threads, not in the Guest.
all: sina-cs-scale.so
sina-cs-scale.so: sina-cs-api/scale.c ../apps/sina-cs-api/include/cs_kernel.h
	$(CC) $(SO_CFLAGS) -std=gnu11 -I../apps/sina-cs-api/include -shared -o $@ $<
