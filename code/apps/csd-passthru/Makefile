CC ?= gcc
CXX ?= c++
CFLAGS ?= -Wall -Wextra -O2 -g
SO_CFLAGS ?= -Wall -Wextra -O2 -g -fPIC
CXX_SO_CFLAGS ?= -Wall -Wextra -O2 -g -fPIC

TARGETS := csd-passthru

.PHONY: all clean

all: $(TARGETS)

csd-passthru: csd-passthru.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
