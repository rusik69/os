# ── Scripts Makefile — host tools build ──────────────────────────────
#
# Builds host-side tools used during kernel configuration and build:
#   mconf  — Kconfig-based menuconfig tool
#
# These are compiled with the host CC (not the cross-compiler).

HOST_CC ?= gcc
HOST_CFLAGS ?= -Wall -Wextra -Wundef -Wmaybe-uninitialized -O2 -g
BUILDDIR ?= ../build/host_tools

# ── mconf — Kconfig menuconfig ──────────────────────────────────────

MCONF_SRC  = kconfig/mconf.c
MCONF_BIN  = $(BUILDDIR)/mconf
MCONF_CFLAGS = $(HOST_CFLAGS)

$(MCONF_BIN): $(MCONF_SRC)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(MCONF_CFLAGS) -o $@ $<

# ── Targets ───────────────────────────────────────────────────────────

.PHONY: all clean mconf

all: mconf

mconf: $(MCONF_BIN)

clean:
	rm -rf $(BUILDDIR)

# ── Install ───────────────────────────────────────────────────────────

install: mconf
	@echo "Host tools built: $(MCONF_BIN)"
