# Makefile for advfs-fuse / advfs-tool
#
# Copyright (C) 2026 Armas Spann
# SPDX-License-Identifier: GPL-2.0-only

CC      ?= gcc
CFLAGS  := -Wall -Wextra -Werror -std=c11 -O2 -D_FILE_OFFSET_BITS=64

PKG_CONFIG ?= pkg-config

# FUSE detection: prefer libfuse3, fall back to libfuse2, then to a
# bare header probe (-lfuse). When no FUSE development files are
# found, "make" builds the core objects only and explains what to
# install; the FUSE sources (main.c, fuse_ops.c) stay untouched and
# ready.
FUSE3_LIBS   := $(shell $(PKG_CONFIG) --libs fuse3 2>/dev/null)
FUSE2_LIBS   := $(shell $(PKG_CONFIG) --libs fuse 2>/dev/null)

ifneq ($(FUSE3_LIBS),)
  FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse3 2>/dev/null) -DADVFS_FUSE3
  FUSE_LIBS   := $(FUSE3_LIBS)
  HAVE_FUSE   := 1
else ifneq ($(FUSE2_LIBS),)
  FUSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags fuse 2>/dev/null)
  FUSE_LIBS   := $(FUSE2_LIBS)
  HAVE_FUSE   := 1
else ifneq ($(wildcard /usr/include/fuse3/fuse.h),)
  FUSE_CFLAGS := -I/usr/include/fuse3 -DADVFS_FUSE3
  FUSE_LIBS   := -lfuse3
  HAVE_FUSE   := 1
else ifneq ($(wildcard /usr/include/fuse.h),)
  FUSE_CFLAGS :=
  FUSE_LIBS   := -lfuse
  HAVE_FUSE   := 1
else
  HAVE_FUSE   := 0
endif

# Core sources have no FUSE dependency; the tests build against them.
CORE_SRC := src/volume.c src/util.c src/bmt.c src/domain.c \
            src/extents.c src/fileset.c src/dir.c src/filedata.c
FUSE_SRC := src/fuse_ops.c src/main.c
TOOL_SRC := src/advfs_tool.c

CORE_OBJ := $(CORE_SRC:.c=.o)
FUSE_OBJ := $(FUSE_SRC:.c=.o)
TOOL_OBJ := $(TOOL_SRC:.c=.o)

BIN      := advfs-fuse
TOOL_BIN := advfs-tool
PREFIX   ?= /usr/local

.PHONY: all core tool fuse nofuse-note clean install test

# "make" builds the CLI tool always and the FUSE driver when FUSE
# development files are present. "make tool" / "make fuse" build one
# binary only (e.g. Gentoo USE-flag driven tool-only builds).
ifeq ($(HAVE_FUSE),1)
all: $(TOOL_BIN) $(BIN)
else
all: $(TOOL_BIN) nofuse-note
endif

core: $(CORE_OBJ)

tool: $(TOOL_BIN)

ifeq ($(HAVE_FUSE),1)
fuse: $(BIN)
else
fuse:
	@echo "ERROR: FUSE development files not found; cannot build $(BIN)." >&2
	@echo "       Install libfuse-dev (FUSE 2) or libfuse3-dev." >&2
	@exit 1
endif

nofuse-note:
	@echo "NOTE: FUSE development files not found (tried pkg-config"
	@echo "      fuse3/fuse and /usr/include/fuse.h)."
	@echo "      Built $(TOOL_BIN) only. Install libfuse-dev (FUSE 2)"
	@echo "      or libfuse3-dev and re-run make to build $(BIN)."

$(BIN): $(CORE_OBJ) $(FUSE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(FUSE_LIBS) -lpthread

# The CLI tool links against the core objects only -- no libfuse,
# no pthread.
$(TOOL_BIN): $(CORE_OBJ) $(TOOL_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

src/fuse_ops.o: src/fuse_ops.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c -o $@ $<

src/main.o: src/main.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c -o $@ $<

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Rebuild all objects when any header changes. Coarse but safe:
# stale objects linked against changed struct layouts segfault.
$(CORE_OBJ) $(FUSE_OBJ) $(TOOL_OBJ): $(wildcard src/*.h)

clean:
	rm -f src/*.o $(BIN) $(TOOL_BIN)
	rm -f tests/test_volume tests/test_domain tests/test_fileset \
	      tests/test_dir tests/test_extents tests/test_edge_cases \
	      tests/test_stripe tests/test_filedata tests/test_clone \
	      tests/debug_symlink

ifeq ($(HAVE_FUSE),1)
install: $(BIN) $(TOOL_BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm755 $(TOOL_BIN) $(DESTDIR)$(PREFIX)/bin/$(TOOL_BIN)
else
install: $(TOOL_BIN)
	install -Dm755 $(TOOL_BIN) $(DESTDIR)$(PREFIX)/bin/$(TOOL_BIN)
endif

test:
	@echo "Running tests..."
	@cd tests && sh run_tests.sh
