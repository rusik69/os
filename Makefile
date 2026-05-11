ifeq ($(origin CC), undefined)
CC = x86_64-elf-gcc
else ifeq ($(origin CC), default)
CC = x86_64-elf-gcc
endif
AS = nasm
ifeq ($(origin LD), undefined)
LD = x86_64-elf-ld
else ifeq ($(origin LD), default)
LD = x86_64-elf-ld
endif
ifeq ($(origin OBJCOPY), undefined)
OBJCOPY = x86_64-elf-objcopy
else ifeq ($(origin OBJCOPY), default)
OBJCOPY = x86_64-elf-objcopy
endif

CFLAGS = -std=c17 -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fno-stack-protector -nostdlib -nostdinc -fno-builtin \
         -Wall -Wextra -Isrc/include -Isrc/gui -mcmodel=large -g
ASFLAGS = -f elf64 -g
LDFLAGS = -T linker.ld -nostdlib -z max-page-size=0x1000

BUILDDIR = build

C_SRCS = src/kernel/kernel.c \
         src/kernel/gdt.c \
         src/kernel/idt.c \
         src/kernel/syscall.c \
         src/kernel/vfs.c \
         src/kernel/elf.c \
         src/drivers/vga.c \
         src/drivers/pic.c \
         src/drivers/timer.c \
         src/drivers/keyboard.c \
         src/drivers/serial.c \
         src/drivers/ata.c \
         src/drivers/pci.c \
         src/drivers/blockdev.c \
         src/drivers/e1000.c \
         src/drivers/intel_gpu.c \
         src/drivers/rtc.c \
         src/drivers/mouse.c \
         src/drivers/speaker.c \
         src/drivers/acpi.c \
         src/drivers/ahci.c \
         src/drivers/usb_ehci.c \
         src/drivers/usb_msc.c \
         src/memory/pmm.c \
         src/memory/vmm.c \
         src/memory/heap.c \
         src/process/process.c \
         src/process/scheduler.c \
         src/process/signal.c \
         src/process/users.c \
         src/shell/shell.c \
         src/shell/shell_vars.c \
         src/shell/editor.c \
         src/shell/script.c \
         src/fs/fs.c \
         src/fs/fat32.c \
         src/net/net.c \
         src/net/net_tcp.c \
         src/net/net_udp.c \
         src/net/telnetd.c \
         src/ipc/pipe.c \
         src/lib/string.c \
         src/lib/printf.c \
         src/lib/stdlib.c \
         src/lib/libc.c

ASM_SRCS = src/boot/boot.asm \
           src/kernel/gdt_asm.asm \
           src/kernel/idt_asm.asm \
           src/kernel/syscall_asm.asm \
           src/process/switch.asm

C_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
ASM_OBJS = $(patsubst src/%.asm,$(BUILDDIR)/%.o,$(ASM_SRCS))
CMD_SRCS = $(wildcard src/shell/cmds/*.c)
CMD_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(CMD_SRCS))
APP_SRCS = $(CMD_SRCS) $(wildcard src/apps/*.c)
COMPILER_SRCS = $(wildcard src/compiler/*.c)
COMPILER_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(COMPILER_SRCS))
GUI_SRCS = $(wildcard src/gui/*.c)
GUI_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(GUI_SRCS))
OBJS = $(ASM_OBJS) $(C_OBJS) $(CMD_OBJS) $(COMPILER_OBJS) $(GUI_OBJS)

.PHONY: all run debug clean deps test test-kernel test-serial test-clean check-app-boundary

check-app-boundary:
    @bad=$$(rg --pcre2 -n '^#include "(?!libc\.h|shell_cmds\.h|printf\.h|string\.h|types\.h)' $(APP_SRCS) 2>/dev/null || true); \
    if [ -n "$$bad" ]; then \
        echo "ERROR: App sources may include only libc-facing headers (libc.h/shell_cmds.h/printf.h/string.h/types.h)."; \
        echo "Direct kernel includes found:"; \
        echo "$$bad"; \
        exit 1; \
    fi

all: $(BUILDDIR)/kernel.bin

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR)/kernel.elf: check-app-boundary $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILDDIR)/kernel.bin: $(BUILDDIR)/kernel.elf
	cp $< $@

$(BUILDDIR)/disk.img:
	@mkdir -p $(BUILDDIR)
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null

run: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	sudo qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev vmnet-shared,id=net0 -device e1000,netdev=net0 ; \
	stty sane

# ── Test build (separate output dir, compiled with -DTEST_MODE) ──────────────

BUILDDIR_TEST = build_test
TEST_CFLAGS   = $(CFLAGS) -DTEST_MODE

C_TEST_SRCS  = $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) src/test/test.c
ASM_TEST_SRCS = $(ASM_SRCS)

C_TEST_OBJS  = $(patsubst src/%.c,$(BUILDDIR_TEST)/%.o,$(C_TEST_SRCS))
ASM_TEST_OBJS = $(patsubst src/%.asm,$(BUILDDIR_TEST)/%.o,$(ASM_TEST_SRCS))
TEST_OBJS    = $(ASM_TEST_OBJS) $(C_TEST_OBJS)

$(BUILDDIR_TEST)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BUILDDIR_TEST)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR_TEST)/kernel.elf: check-app-boundary $(TEST_OBJS)
	@mkdir -p $(BUILDDIR_TEST)
	$(LD) $(LDFLAGS) -o $@ $(TEST_OBJS)

$(BUILDDIR_TEST)/kernel.bin: $(BUILDDIR_TEST)/kernel.elf
	cp $< $@

# Build the test kernel binary
test-kernel: $(BUILDDIR_TEST)/kernel.bin

# Run headless QEMU on serial TCP 4444 for manual inspection of test kernel
test-serial: $(BUILDDIR_TEST)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -kernel $(BUILDDIR_TEST)/kernel.bin -m 256M \
		-serial tcp::4444,server,nowait \
		-vga none -display none \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-no-reboot

# Run E2E tests: build test kernel, then boot in QEMU, capture serial, assert results
test: test-kernel $(BUILDDIR)/disk.img
	@chmod +x tests/run_tests.sh
	@./tests/run_tests.sh $(BUILDDIR_TEST)/kernel.bin $(BUILDDIR)/disk.img

# Full clean rebuild + test
test-clean: clean
	$(MAKE) test

# E2E tests: boot normal kernel in QEMU with user-mode networking + telnet hostfwd,
# then drive every shell command via Python telnet client.
e2e: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/e2e.sh tests/e2e.py
	@./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# E2E with explicit telnet port (override default 2323)
e2e-port-%: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/e2e.sh tests/e2e.py
	@E2E_PORT=$* ./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

debug: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	sudo qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std -s -S \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev vmnet-shared,id=net0 -device e1000,netdev=net0

clean:
	rm -rf $(BUILDDIR) $(BUILDDIR_TEST)

deps:
	brew install x86_64-elf-gcc nasm qemu xorriso
