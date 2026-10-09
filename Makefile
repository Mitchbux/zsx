CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNFLAGS ?= -Wall -Wextra -Wpedantic
WITH_TCC ?= 0
TCC_CPPFLAGS ?=
TCC_LDLIBS ?= -ltcc -ldl
ifeq ($(WITH_TCC),1)
CPPFLAGS += -DZSX_WITH_TCC $(TCC_CPPFLAGS)
LDLIBS += $(TCC_LDLIBS)
endif

.PHONY: all clean test

all: zsx

zsx: zsx.c zsx_high.h zsx_x.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -std=c11 -o $@ $< $(LDLIBS)

test: zsx
	haxe buildnode.hxml
	node test/patch_for_node.js
	node test/zsx_range_regression.js
	node test/zsx_c_cross_compat.js ./zsx
	@tmp=$$(mktemp); trap 'rm -f "$$tmp"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -std=c11 zsx_high_test.c -o "$$tmp" && "$$tmp"
	python3 test/zsx_high_cli.py ./zsx

clean:
	rm -f zsx zsx.exe