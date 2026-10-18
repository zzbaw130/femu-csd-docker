# Each program directory owns its sources/rules; all products share one output directory.
BUILD_DIR ?= $(CURDIR)/build
OUT := $(abspath $(BUILD_DIR))
PROGRAMS := $(patsubst %/Makefile,%,$(wildcard */Makefile))
.PHONY: all clean check bpf $(PROGRAMS) csd-vadd.so csd-original-kernels.so \
    sina-cs-scale.so libcs-workmem.so csd-workmem-probe-a.so csd-workmem-probe-b.so
all: $(PROGRAMS)
$(PROGRAMS):
	$(MAKE) -C $@ BUILD_DIR=$(OUT) all
csd-vadd.so csd-original-kernels.so: examples
sina-cs-scale.so: sina-cs-api
libcs-workmem.so csd-workmem-probe-a.so csd-workmem-probe-b.so: csd-workmem
bpf:
	$(MAKE) -C examples BUILD_DIR=$(OUT) bpf
check: all
	$(MAKE) -C csd-workmem BUILD_DIR=$(OUT) check
clean:
	$(foreach program,$(PROGRAMS),$(MAKE) -C $(program) BUILD_DIR=$(OUT) clean &&) true
