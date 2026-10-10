CC ?= cc
CFLAGS ?= -O2
CPPFLAGS ?=
WARNFLAGS ?= -Wall -Wextra -Wpedantic

.PHONY: all clean

all: zsx

zsx: zsx.c zsx_high.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -std=c11 -o $@ $< $(LDLIBS)

clean:
	rm -f zsx zsx.exe