# Include from each native CSF component. The workmem implementation opts out of
# the fallback library recipe to define its own library rule.
.DEFAULT_GOAL := all
CSD_RULES_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
CSD_PROGRAMS_ROOT := $(abspath $(CSD_RULES_DIR)/..)
BUILD_DIR ?= $(CSD_PROGRAMS_ROOT)/build
OUT := $(abspath $(BUILD_DIR))
CSD_WORKMEM_LIB := $(OUT)/libcs-workmem.so
CSF_CPPFLAGS := -I$(CSD_PROGRAMS_ROOT)/csd-workmem/include
# Retain DT_NEEDED even when an operator does not currently allocate workmem.
# Restore the caller's linker state after this dependency.
CSF_LDLIBS := -L$(OUT) -Wl,--push-state,--no-as-needed -lcs-workmem \
              -Wl,--pop-state -Wl,-rpath,'$$ORIGIN'
CSF_BUILD_DEPS := Makefile $(CSD_RULES_DIR)/csf.mk $(CSD_WORKMEM_LIB)
ifneq ($(CSD_WORKMEM_IMPLEMENTATION),1)
$(CSD_WORKMEM_LIB): $(CSD_PROGRAMS_ROOT)/csd-workmem/src/workmem.c \
    $(CSD_PROGRAMS_ROOT)/csd-workmem/include/cs_workmem.h \
    $(CSD_PROGRAMS_ROOT)/csd-workmem/src/exports.map \
    $(CSD_PROGRAMS_ROOT)/csd-workmem/Makefile
	$(MAKE) -C $(CSD_PROGRAMS_ROOT)/csd-workmem BUILD_DIR=$(OUT) library
endif
