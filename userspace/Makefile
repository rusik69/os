# ── Userspace build ──────────────────────────────────────────────
# Builds init.elf with static libc, then packages as initramfs.cpio
# Uses x86_64-linux-gnu toolchain (not the kernel's cross compiler)
CROSS_COMPILE ?= x86_64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar

LIBC_DIR = libc
LIBC_A = $(LIBC_DIR)/libc.a

CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -mno-red-zone \
         -I$(LIBC_DIR)/include -Wall -Wextra -Werror -O2 -g -MMD -MP
LDFLAGS = -T $(LIBC_DIR)/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack

LIBC_OBJS = \
	$(LIBC_DIR)/crt0.o \
	$(LIBC_DIR)/syscall.o \
	$(LIBC_DIR)/stdio.o \
	$(LIBC_DIR)/string.o \
	$(LIBC_DIR)/stdlib.o

all: initramfs.cpio

# libc archive
$(LIBC_A): $(LIBC_OBJS)
	$(AR) rcs $@ $^

# init program
init/init.o: init/init.c
	$(CC) $(CFLAGS) -c $< -o $@

init.elf: init/init.o $(LIBC_A)
	$(LD) $(LDFLAGS) init/init.o $(LIBC_A) -o $@

# CPIO archive containing /bin/sh (our init.elf)
initramfs.cpio: init.elf
	@rm -rf initramfs_root
	@mkdir -p initramfs_root/bin
	cp init.elf initramfs_root/bin/sh
	cd initramfs_root && \
		find . | cpio -o -H newc --quiet > ../initramfs.cpio
	@rm -rf initramfs_root
	@echo "[userspace] Created initramfs.cpio"

clean:
	rm -f $(LIBC_OBJS) $(LIBC_A) init/init.o init.elf initramfs.cpio

-include init/init.d

.PHONY: all clean

.SUFFIXES:
.SUFFIXES: .c .o .S .elf .cpio

$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.s
	$(AS) $< -o $@

$(LIBC_DIR)/%.o: $(LIBC_DIR)/%.S
	$(CC) $(CFLAGS) -c $< -o $@
