# ── Userspace build ──────────────────────────────────────────────
# Builds libc.a, init.elf, sh.elf, and all command ELFs,
# then packages as initramfs.cpio
# Uses x86_64-linux-gnu toolchain (not the kernel's cross compiler)
CROSS_COMPILE ?= x86_64-elf-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
STRIP_CMD := $(CROSS_COMPILE)strip

LIBC_DIR = libc
LIBC_A = $(LIBC_DIR)/libc.a

CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -mno-red-zone \
         -I$(LIBC_DIR)/include -Wall -Wextra -Werror -O2 -g -MMD -MP
LDFLAGS = -T $(LIBC_DIR)/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack

LIBC_OBJS = \
	$(LIBC_DIR)/crt0.o \
	$(LIBC_DIR)/syscall.o \
	$(LIBC_DIR)/sys.o \
	$(LIBC_DIR)/stdio.o \
	$(LIBC_DIR)/string.o \
	$(LIBC_DIR)/stdlib.o

# All binaries to build
BIN_ELFS = init.elf sh.elf init2.elf initramfs.elf doom.elf dos.elf gui.elf

# Command ELFs
CMD_SRCS = $(wildcard bin/cmds/*.c)
CMD_ELFS = $(patsubst bin/cmds/%.c, %.elf, $(CMD_SRCS))

.PHONY: all clean

all: initramfs.cpio

# libc archive
$(LIBC_A): $(LIBC_OBJS)
	$(AR) rcs $@ $^

# Pattern rules for building .o files
$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.s
	$(AS) $< -o $@

$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.S
	$(CC) $(CFLAGS) -c $< -o $@

# init program
init/init.o: init/init.c
	$(CC) $(CFLAGS) -c $< -o $@

init.elf: init/init.o $(LIBC_A)
	$(LD) $(LDFLAGS) init/init.o $(LIBC_A) -o $@

# Shell
bin/sh.o: bin/sh.c
	$(CC) $(CFLAGS) -c $< -o $@

sh.elf: bin/sh.o $(LIBC_A)
	$(LD) $(LDFLAGS) bin/sh.o $(LIBC_A) -o $@

# init2 — standalone userspace init with inittab, runlevels, service supervision (no libc)
init2/init.o: init2/init.c
	$(CC) $(CFLAGS) -c $< -o $@

init2.elf: init2/init.o
	$(LD) $(LDFLAGS) init2/init.o -o $@

# initramfs — standalone initramfs init (no libc)
initramfs/initramfs.o: initramfs/initramfs.c
	$(CC) $(CFLAGS) -c $< -o $@

initramfs.elf: initramfs/initramfs.o
	$(LD) $(LDFLAGS) initramfs/initramfs.o -o $@

# Command ELFs — each is a standalone userspace binary
bin/cmds/%.o: bin/cmds/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.elf: bin/cmds/%.o $(LIBC_A)
	$(LD) $(LDFLAGS) $< $(LIBC_A) -o $@

# Doom — standalone userspace game
DOOM_SRCS = doom/doom_task.c doom/doom_render.c doom/doom_map.c doom/doom_player.c \
            doom/doom_combat.c doom/doom_math.c doom/doom_raycast.c \
            doom/doom_textures.c doom/doom_sprites.c doom/doom_doors.c doom/doom_floor.c
DOOM_OBJS = $(patsubst doom/%.c,doom/%.o,$(DOOM_SRCS))

doom/%.o: doom/%.c
	$(CC) $(CFLAGS) -c $< -o $@

doom.elf: $(DOOM_OBJS) $(LIBC_A)
	$(LD) $(LDFLAGS) $^ -o $@

# DOS emulator — standalone userspace app
DOS_SRCS = dos/main.c dos/dos_emu.c dos/dos_int21.c dos/dos_ints.c dos/dos_load.c
DOS_OBJS = $(patsubst dos/%.c,dos/%.o,$(DOS_SRCS))

dos/%.o: dos/%.c
	$(CC) $(CFLAGS) -c $< -o $@

dos.elf: $(DOS_OBJS) $(LIBC_A)
	$(LD) $(LDFLAGS) $^ -o $@

# GUI desktop — standalone userspace app
GUI_SRCS = gui/gui_task.c gui/gui.c gui/gui_widgets.c gui/gui_shell.c
GUI_OBJS = $(patsubst gui/%.c,gui/%.o,$(GUI_SRCS))

gui/%.o: gui/%.c
	$(CC) $(CFLAGS) -c $< -o $@

gui.elf: $(GUI_OBJS) $(LIBC_A)
	$(LD) $(LDFLAGS) $^ -o $@

# Initramfs packaging
initramfs.cpio: $(BIN_ELFS) $(CMD_ELFS)
	@rm -rf initramfs_root
	@mkdir -p initramfs_root/bin initramfs_root/sbin
	cp init.elf initramfs_root/init
	cp init2.elf initramfs_root/sbin/init2
	cp initramfs.elf initramfs_root/initramfs
	cp sh.elf initramfs_root/bin/sh
	cp doom.elf initramfs_root/bin/doom
	cp dos.elf initramfs_root/bin/dos
	cp gui.elf initramfs_root/bin/gui
	@for f in $(CMD_ELFS); do \
		name=$${f%.elf}; \
		cp $$f initramfs_root/bin/$$name; \
	done
	@# Strip ELF binaries to reduce initramfs size
	@if command -v $(STRIP_CMD) >/dev/null 2>&1; then \
		echo "[userspace] Stripping ELF binaries..."; \
		$(STRIP_CMD) initramfs_root/init initramfs_root/sbin/init2 initramfs_root/initramfs initramfs_root/bin/* 2>/dev/null || true; \
	else \
		echo "[userspace] $(STRIP_CMD) not found, skipping strip"; \
	fi
	cd initramfs_root && \
		find . | cpio -o -H newc --quiet > ../initramfs.cpio
	@rm -rf initramfs_root
	@echo "[userspace] Created initramfs.cpio"

clean:
	rm -f $(LIBC_OBJS) $(LIBC_A)
	rm -f init/init.o init.elf
	rm -f init2/init.o init2.elf
	rm -f initramfs/initramfs.o initramfs.elf
	rm -f bin/sh.o
	rm -f $(patsubst %.elf,bin/cmds/%.o,$(CMD_ELFS))
	rm -f $(DOOM_OBJS) doom.elf
	rm -f $(DOS_OBJS) dos.elf
	rm -f $(GUI_OBJS) gui.elf
	rm -f $(BIN_ELFS) $(CMD_ELFS)
	rm -f initramfs.cpio

-include init/init.d
-include $(LIBC_DIR)/stdio.d
-include $(LIBC_DIR)/string.d
-include $(LIBC_DIR)/stdlib.d
-include $(LIBC_DIR)/syscall.d

.SUFFIXES:
.SUFFIXES: .c .o .S .elf .cpio

# vpath for command sources
vpath %.c bin/cmds
