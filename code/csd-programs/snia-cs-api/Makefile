CC ?= cc
include ../mk/csf.mk
CPPFLAGS += $(CSF_CPPFLAGS)
SO_CFLAGS ?= -O2 -g -Wall -Wextra -Werror -fPIC -pthread
SDK_INCLUDE := ../../apps/snia-cs-api/plugins/femu/include
.PHONY: all clean
all: $(OUT)/snia-cs-scale.so
$(OUT):
	mkdir -p $@
$(OUT)/snia-cs-scale.so: scale.c $(SDK_INCLUDE)/femu-csd-kernel.h $(SDK_INCLUDE)/cs_csf.h $(CSF_BUILD_DEPS) | $(OUT)
	$(CC) $(CPPFLAGS) $(SO_CFLAGS) -std=gnu11 -I$(SDK_INCLUDE) -shared -o $@.tmp $< $(CSF_LDLIBS)
	mv -f $@.tmp $@
clean:
	rm -f $(OUT)/snia-cs-scale.so
