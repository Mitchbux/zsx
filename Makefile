CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNFLAGS ?= -Wall -Wextra -Wpedantic

.PHONY: all clean test

all: zsx

zsx: zsx.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -std=c11 -o $@ $<

test: zsx
	haxe buildnode.hxml
	node test/patch_for_node.js
	node test/zsx_range_regression.js
	node test/zsx_c_cross_compat.js ./zsx

clean:
	rm -f zsx zsx.exe