# Makefile for Lhassa - GNU make, AmigaOS 68k via vbcc
#
# Default build uses vbcc (vc + vlink ? Hunk executable).
# Host smoke-test only: make HOST=1

MAKEFLAGS += -r

CPPFLAGS += -Iinclude -Isrc -Icli
AR      ?= ar
LHA     = lha

CLISRC = cli/lha.c cli/lha_parse.c cli/lha_util.c cli/lha_cmds.c cli/lha_amiga.c
CLIOBJ = $(CLISRC:.c=.o)

ifeq ($(HOST),1)
CC      := cc
CFLAGS  := -std=c89 -pedantic -Wall -Wno-unused-function -O2 -DHOST=1
LDFLAGS :=
else
override CC := vc
CFLAGS  := +aos68k_posix -O2
LDFLAGS := +aos68k_posix -Lstack=65536
endif

COMPILE = $(CC) $(CFLAGS) $(CPPFLAGS) -c -o
LINK    = $(CC) $(LDFLAGS) -o

LIBSRC = \
	src/lh_error.c \
	src/lh_crc.c \
	src/lh_crypto.c \
	src/lh_datetime.c \
	src/lh_codec.c \
	src/lh_lh5dec.c \
	src/lh_lh1dec.c \
	src/lh_header.c \
	src/lh_archive.c \
	src/lh_buffer.c \
	src/lh_lzhuf.c

ifneq ($(HOST),1)
# vbcc 68000: codec % emits __lmodu; C entry is _lmodu (see src/lmodu.c).
LIBSRC += src/lmodu.c
endif

LIBOBJ = $(LIBSRC:.c=.o)
TEST   = test/smoke

.PHONY: all clean test show

all: show $(LHA) liblh.a

show:
	@echo "CC=$(CC)"
	@echo "COMPILE=$(COMPILE)"

liblh.a: $(LIBOBJ)
	-rm -f $@
	$(AR) q $@ $(LIBOBJ)

$(LHA): $(CLIOBJ) $(LIBOBJ)
	$(LINK) $@ $(CLIOBJ) $(LIBOBJ)

ifeq ($(HOST),1)
test: $(TEST)
	./$(TEST)

$(TEST): test/smoke.c $(LIBOBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test/smoke.c $(LIBOBJ)
endif

$(LIBOBJ): %.o: %.c src/lh_internal.h include/lh.h
	$(COMPILE) $@ $<

$(CLIOBJ): %.o: %.c include/lh.h cli/lha_internal.h
	$(COMPILE) $@ $<

clean:
	-rm -f $(LIBOBJ) $(CLIOBJ) liblh.a $(LHA) $(TEST)
