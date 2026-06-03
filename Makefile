# ── Compiler / Toolchain ──────────────────────────────────────────────

ifeq ($(origin CC), undefined)
CC = x86_64-elf-gcc
else ifeq ($(origin CC), default)
CC = x86_64-elf-gcc
endif

# Auto-detect ccache and wrap CC if available — massively speeds up rebuilds
ifneq ($(shell which ccache 2>/dev/null),)
  CC := ccache $(CC)
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

# Number of parallel jobs (all available CPU cores)
NPROCS := $(shell nproc)

# Kernel version string — used in module vermagic checking
KVERSION ?= 6.1.0-osdev

# Determine preemption model for vermagic
ifdef CONFIG_PREEMPT
VERMAGIC_FLAGS := -DCONFIG_PREEMPT
else ifdef CONFIG_PREEMPT_VOLUNTARY
VERMAGIC_FLAGS := -DCONFIG_PREEMPT_VOLUNTARY
else
VERMAGIC_FLAGS :=
endif

# SMP enabled? (auto-detected from target)
VERMAGIC_FLAGS += -DCONFIG_SMP

CFLAGS = -std=c17 -ffreestanding -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -fstack-protector-strong -mstack-protector-guard=global -fno-omit-frame-pointer -nostdlib -nostdinc -fno-builtin \
         -Wall -Wextra -Isrc/include -Isrc/gui -Isrc/doom -mcmodel=large -g \
         -Wa,--noexecstack -O2 -MMD -MP \
         -DKVERSION=\"$(KVERSION)\" $(VERMAGIC_FLAGS) \
         $(CFLAGS_EXTRA)
ASFLAGS = -f elf64 -g
LDFLAGS = -T linker.ld -nostdlib -z max-page-size=0x1000 -z noexecstack

BUILDDIR = build

C_SRCS = src/kernel/kernel.c \
         src/kernel/gdt.c \
         src/kernel/idt.c \
         src/kernel/fault.c \
         src/kernel/syscall.c \
         src/kernel/vfs.c \
         src/kernel/elf.c \
         src/kernel/apic.c \
         src/kernel/cmdline.c \
         src/kernel/smp.c \
         src/kernel/cpu.c \
         src/kernel/cpuidle.c \
         src/kernel/oom.c \
         src/kernel/rcu.c \
         src/kernel/aslr.c \
         src/kernel/seccomp.c \
         src/kernel/sysrq.c \
         src/kernel/panic.c \
         src/kernel/nmi_watchdog.c \
         src/kernel/lockdep.c \
         src/kernel/timers.c \
         src/kernel/workqueue.c \
         src/kernel/idr.c \
         src/kernel/rng.c \
         src/kernel/fsnotify.c \
         src/kernel/module.c \
         src/kernel/module_elf.c \
         src/kernel/module_signature.c \
         src/kernel/module_autoload.c \
         src/kernel/module_deps.c \
         src/kernel/module_alias.c \
         src/kernel/sysctl.c \
         src/kernel/ksym.c \
         src/kernel/caps.c \
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
         src/drivers/acpi_thermal.c \
         src/drivers/acpi_ec.c \
         src/power/suspend.c \
         src/drivers/ahci.c \
         src/drivers/nvme.c \
         src/drivers/usb_ehci.c \
         src/drivers/usb_msc.c \
         src/drivers/ps2.c \
         src/drivers/fbcon.c \
         src/drivers/partitions.c \
         src/drivers/mdadm.c \
         src/drivers/cmos.c \
         src/drivers/floppy.c \
         src/drivers/battery.c \
         src/drivers/i2c.c \
         src/memory/pmm.c \
         src/memory/vmm.c \
         src/memory/heap.c \
         src/memory/slab.c \
         src/memory/compaction.c \
         src/process/process.c \
         src/process/scheduler.c \
         src/process/sched_deadline.c \
         src/process/pelt.c \
         src/process/signal.c \
         src/process/users.c \
         src/shell/shell.c \
         src/shell/shell_cmd_table.c \
         src/shell/shell_vars.c \
         src/shell/editor.c \
         src/shell/syntax.c \
         src/shell/script.c \
         src/fs/fs.c \
         src/fs/procfs.c \
         src/fs/devfs.c \
         src/fs/tmpfs.c \
         src/ipc/shm.c \
         src/fs/fat32.c \
         src/fs/iso9660.c \
         src/fs/bufcache.c \
         src/fs/page_cache.c \
         src/fs/fstab.c \
         src/net/net.c \
         src/net/net_tcp.c \
         src/net/net_udp.c \
         src/net/ipv6.c \
         src/net/telnetd.c \
         src/net/httpd.c \
         src/net/socket.c \
         src/net/netfilter.c \
         src/net/conntrack.c \
         src/net/pkt_sched.c \
         src/net/bridge.c \
         src/net/vlan.c \
         src/net/tun.c \
         src/net/net_ns.c \
         src/net/ipip.c \
         src/net/wireguard.c \
         src/net/ipvs.c \
         src/net/sshd.c \
         src/kernel/service.c \
         src/kernel/ssh_crypto.c \
         src/kernel/ssh_client.c \
         src/ipc/pipe.c \
         src/ipc/eventfd.c \
         src/drivers/virtio_net.c \
         src/drivers/virtio_blk.c \
         src/drivers/ac97.c \
         src/drivers/ramdisk.c \
         src/drivers/watchdog.c \
         src/ipc/mutex.c \
         src/ipc/fifo.c \
         src/ipc/mqueue.c \
         src/fs/sysfs.c \
         src/fs/debugfs.c \
         src/ipc/semaphore.c \
         src/kernel/audit.c \
         src/kernel/yama.c \
         src/kernel/kptr_restrict.c \
         src/kernel/dmesg.c \
         src/ipc/waitqueue.c \
         src/dos/dos_emu.c \
         src/dos/dos_ints.c \
         src/dos/dos_int21.c \
         src/dos/dos_load.c \
         src/lib/string.c \
         src/lib/printf.c \
         src/lib/stdlib.c \
         src/lib/libc.c \
         src/lib/stdio.c \
         src/lib/unistd.c \
         src/lib/signal_libc.c \
         src/lib/pthread.c \
         src/lib/dlfcn.c \
         src/drivers/xattr.c \
         src/drivers/file_lock.c \
         src/drivers/kaps.c \
         src/drivers/pgrp.c \
         src/drivers/coredump.c \
         src/drivers/aio.c \
         src/drivers/signalfd.c \
         src/drivers/timerfd.c \
         src/drivers/hpet.c \
         src/drivers/dmi.c \
         src/drivers/bochs.c \
         src/drivers/loop.c \
         src/drivers/dyndbg.c \
         src/drivers/uio.c \
         src/drivers/pagecache.c \
         src/fs/freeze.c \
         src/fs/quota.c \
         src/fs/crypto.c \
         src/net/dhcp.c \
         src/kernel/irq_affinity.c \
         src/kernel/trace.c \
         src/kernel/smap_smep_umip.c \
         src/kernel/notifier.c \
         src/kernel/softirq.c \
         src/kernel/tasklet.c \
         src/kernel/stacktrace.c \
         src/kernel/ratelimit.c \
         src/kernel/logbuf.c \
         src/lib/crc32.c \
         src/lib/uuid.c \
         src/lib/hexdump.c \
         src/lib/radix_tree.c \
         src/lib/bitmap.c \
         src/lib/find_bit.c \
         src/lib/mempool.c \
         src/lib/crc16.c \
         src/lib/string_ext.c \
         src/lib/time.c \
         src/lib/stdlib_ext.c \
         src/lib/search.c \
         src/lib/errno_ext.c \
         src/lib/assert.c \
         src/lib/base64.c \
         src/lib/aes.c \
         src/lib/sha256.c \
         src/lib/sha512.c \
         src/lib/md5.c \
         src/lib/hmac.c \
         src/lib/crc64.c \
         src/lib/adler32.c \
         src/kernel/rwsem.c \
         src/kernel/x2apic.c \
         src/kernel/tsc_deadline.c \
         src/kernel/invpcid.c \
         src/kernel/fsgsbase.c \
         src/kernel/rdpid.c \
         src/kernel/nx_enforce.c \
         src/kernel/vsyscall.c \
         src/memory/memhotplug.c \
         src/memory/page_poison.c \
         src/memory/cma.c \
         src/memory/zram.c \
         src/memory/ksm.c \
         src/memory/thp.c \
         src/memory/zcomp.c \
         src/memory/zcomp_fast.c \
         src/kernel/perf_events.c \
         src/kernel/jump_label.c \
         src/kernel/pstore.c \
         src/kernel/kdump.c \
         src/kernel/stack_guard.c \
         src/kernel/rseq.c \
         src/kernel/mce.c \
         src/kernel/kasan_light.c \
         src/kernel/compress.c \
         src/kernel/firmware.c \
         src/kernel/memfd.c \
         src/kernel/mseal.c \
         src/kernel/userfaultfd.c \
         src/kernel/madvise_ext.c \
         src/kernel/mem_policy.c \
         src/kernel/page_idle.c \
         src/kernel/page_allocator_ext.c \
         src/kernel/sched_attr.c \
         src/kernel/cpuset.c \
         src/kernel/pidfd.c \
         src/kernel/landlock.c \
         src/kernel/seccomp_bpf.c \
         src/kernel/process_rlimit.c \
         src/kernel/devtmpfs.c \
         src/kernel/overlay.c \
         src/kernel/fanotify.c \
         src/kernel/fs_mount_prop.c \
         src/kernel/net_igmp.c \
         src/kernel/net_lldp.c \
         src/kernel/aio_enhanced.c \
         src/kernel/range.c \
         src/kernel/hashtable.c \
         src/kernel/interval_tree.c \
         src/kernel/prio_tree.c \
         src/kernel/bitfield.c \
         src/kernel/hweight.c \
         src/kernel/sort_ext.c \
         src/kernel/timer_source.c \
         src/kernel/klist.c \
         src/kernel/cpu_bitmask.c \
         src/kernel/cpu_topology.c \
         src/kernel/llist.c \
         src/kernel/percpu_counter.c \
         src/kernel/ratelimit_ext.c \
         src/kernel/refcount_ext.c \
         src/kernel/dynamic_debug.c \
         src/kernel/irq_work.c \
         src/kernel/taskstats.c \
         src/kernel/irq_regs.c \
         src/kernel/timeconst.c \
         src/kernel/div64.c \
         src/kernel/gpiolib.c \
         src/kernel/io_map.c \
         src/kernel/config_gz.c \
         src/kernel/fault_inject.c

ASM_SRCS = src/boot/boot.asm \
           src/kernel/gdt_asm.asm \
           src/kernel/idt_asm.asm \
           src/kernel/syscall_asm.asm \
           src/process/switch.asm \
           src/kernel/ap_trampoline.asm

C_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(C_SRCS))
ASM_OBJS = $(patsubst src/%.asm,$(BUILDDIR)/%.o,$(ASM_SRCS))
CMD_SRCS = $(wildcard src/shell/cmds/*.c)
CMD_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(CMD_SRCS))
APP_SRCS = $(CMD_SRCS) $(wildcard src/apps/*.c)
COMPILER_SRCS = $(wildcard src/compiler/*.c)
COMPILER_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(COMPILER_SRCS))
GUI_SRCS = $(wildcard src/gui/*.c)
GUI_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(GUI_SRCS))
DOOM_SRCS = $(wildcard src/doom/*.c)
DOOM_OBJS = $(patsubst src/%.c,$(BUILDDIR)/%.o,$(DOOM_SRCS))
OBJS = $(ASM_OBJS) $(C_OBJS) $(CMD_OBJS) $(COMPILER_OBJS) $(GUI_OBJS) $(DOOM_OBJS)
# Header dependency tracking: include .d files when they exist
DEPS = $(C_OBJS:.o=.d) $(CMD_OBJS:.o=.d) $(COMPILER_OBJS:.o=.d) $(GUI_OBJS:.o=.d) $(DOOM_OBJS:.o=.d)

# ── Kernel module build rules (M39) ──────────────────────────────────
# Loadable kernel modules (.ko) are compiled with -DMODULE and partially
# linked via `ld -r` to produce relocatable files for the module loader.
#
# Module source files live alongside the regular kernel sources in src/.
# obj-m lists the module .ko names to build (without path or extension).
# Each module .ko is built from one or more .c source files compiled with
# -DMODULE, then partially linked together.
#
# Example usage:
#   make modules          # build all modules listed in obj-m
#   make modules_install  # copy .ko files into the disk image
#
# Module compilation flags: same as CFLAGS but with -DMODULE and without
# -nostdinc (modules include kernel headers via the same include paths).
MODULE_CFLAGS  = $(filter-out -nostdinc -fno-builtin -mno-mmx -mno-sse -mno-sse2, $(CFLAGS)) \
                 -DMODULE -ffreestanding -nostdlib -fno-builtin -Isrc/include
MODULE_LDFLAGS = -r -z max-page-size=0x1000

# Build directory for module objects and .ko files
MODULE_BUILDDIR = $(BUILDDIR)/modules

# obj-m lists module .ko names to build (relative to MODULE_BUILDDIR, .ko suffix)
# Each entry creates a build target: $(MODULE_BUILDDIR)/<name>.ko
# Override this in a submake or set in environment to add custom modules.
obj-m ?= drivers/e1000.ko

# Derive module .ko paths from obj-m list
MODULE_KOS = $(addprefix $(MODULE_BUILDDIR)/, $(obj-m))

# Module .o files: for each <name>.ko, we need at least one <name>.o
# The source file for <name>.o is found by searching src/ for <name>.c
# This supports flat and hierarchical source layouts.
MODULE_OBJS = $(foreach ko,$(obj-m), \
    $(MODULE_BUILDDIR)/$(basename $(ko)).o )

# Rule to compile a module source file into a module .o file
# Source file is located under src/ relative to the .ko name
# e.g., build/modules/drivers/e1000.ko ← src/drivers/e1000.c
$(MODULE_BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODULE_CFLAGS) -c $< -o $@

# Rule to partially-link module .o(s) into a .ko relocatable file
# Supports multi-source modules: <name>.ko can depend on multiple .o files
$(MODULE_BUILDDIR)/%.ko: $(MODULE_BUILDDIR)/%.o
	@mkdir -p $(dir $@)
	$(LD) $(MODULE_LDFLAGS) -o $@ $^

# Build all kernel modules listed in obj-m
modules: $(MODULE_KOS)

# Install built modules into the filesystem disk image
# Copies .ko files to /modules/ on the FAT32 disk image (where the
# kernel's module loader expects to find them at runtime).
modules_install: modules
	@echo "=== Installing modules ==="
	@mkdir -p /tmp/modules_staging
	@for ko in $(MODULE_KOS); do \
	    if [ -f "$$ko" ]; then \
	        name=$$(basename $$ko); \
	        echo "  INSTALL $$name"; \
	        cp "$$ko" /tmp/modules_staging/$$name; \
	    fi; \
	done
	@echo "Modules staged in /tmp/modules_staging/"
	@echo "Run: scripts/install_modules.sh <disk_image> to add to disk image"

# ── /proc/config.gz — embedded compressed kernel build config ──────
#
# Pipeline: gen_config.sh → build_config.txt → gzip → build_config.gz
#           → xxd -i → build/build_config_gz.h (included by config_gz.c)
#
BUILD_CONFIG_TXT  = $(BUILDDIR)/build_config.txt
BUILD_CONFIG_GZ   = $(BUILDDIR)/build_config.gz
BUILD_CONFIG_GZ_H = $(BUILDDIR)/build_config_gz.h

# The header must be generated before config_gz.c is compiled.
# We add the build directory as an include path so #include "build_config_gz.h" resolves.
$(BUILDDIR)/kernel/config_gz.o: CFLAGS += -I$(BUILDDIR)

# Generate the header: config text → gzip → xxd -i → header
# Write to a temp file and atomically rename to avoid parallel-build races
$(BUILD_CONFIG_GZ_H): $(BUILD_CONFIG_GZ)
	@mkdir -p $(dir $@)
	@{ \
	    echo '/* Auto-generated — do not edit. */'; \
	    echo '#ifndef BUILD_CONFIG_GZ_H'; \
	    echo '#define BUILD_CONFIG_GZ_H'; \
	    xxd -i -n build_config_gz < $<; \
	    echo '#endif /* BUILD_CONFIG_GZ_H */'; \
	} > $@.tmp && mv $@.tmp $@

$(BUILD_CONFIG_GZ): $(BUILD_CONFIG_TXT)
	gzip -c < $< > $@

$(BUILD_CONFIG_TXT): scripts/gen_config.sh
	@mkdir -p $(dir $@)
	scripts/gen_config.sh $@

# ── Default target: build kernel in parallel ──────────────────────────
# NOTE: -include must stay BELOW the default target so that dependency
# files never accidentally steal .DEFAULT_GOAL.

.DEFAULT_GOAL := all

all: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin

-include $(wildcard $(DEPS))

# ── Phony targets ─────────────────────────────────────────────────────

.PHONY: all run debug clean deps test test-kernel test-serial test-clean clean-all \
        check check-clean check-app-boundary doom-test format format-check lint ccache-stats count build-info run-test unit-test bench \
        modules modules_install

# ── Boundary check on app sources ─────────────────────────────────────

check-app-boundary:
	@bad=$$(rg --pcre2 -n '^#include "(?!libc\.h|shell_cmds\.h|shell_cmd_table\.h|shell\.h|printf\.h|string\.h|stdlib\.h|types\.h|keyboard\.h|blockdev\.h|fat32\.h|ata\.h|ahci\.h|service\.h|fault\.h|syscall\.h|vfs\.h|module\.h|module_elf\.h|heap\.h|ssh\.h|ssh_client\.h|vfs\.h|sysctl\.h|users\.h|net\.h|fstab\.h|devtmpfs\.h|nvme\.h|vga\.h)' $(APP_SRCS) 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
	    echo "ERROR: App source includes an unexpected header."; \
	    echo "Allowed headers: libc.h, shell_cmds.h, shell_cmd_table.h, shell.h, printf.h,"; \
	    echo "  string.h, stdlib.h, types.h, keyboard.h, blockdev.h, fat32.h, ata.h,"; \
	    echo "  ahci.h, service.h, fault.h, heap.h, devtmpfs.h, nvme.h"; \
	    echo "Offending files:"; \
	    echo "$$bad"; \
	    exit 1; \
	fi

# ── Compilation rules ─────────────────────────────────────────────────

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR)/kernel.elf: check-app-boundary $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) -L/usr/lib/gcc/x86_64-linux-gnu/13 -lgcc

# config_gz.o depends on the auto-generated header
$(BUILDDIR)/kernel/config_gz.o: $(BUILD_CONFIG_GZ_H)

$(BUILDDIR)/kernel.bin: $(BUILDDIR)/kernel.elf
	cp $< $@

# ── Userspace init binary (standalone ELF for kernel loader) ─────────

# Userspace programs need different flags: PIE, no kernel restrictions,
# but still freestanding (no libc).  The init.ld linker script produces
# a raw ELF with proper phdrs for the kernel's ELF loader.
INIT_CFLAGS = -std=gnu17 -ffreestanding -nostdlib -nostdinc -fno-builtin \
              -fno-stack-protector -mcmodel=large -O2 \
              -Wall -Wextra -Isrc/include -g

$(BUILDDIR)/init.o: src/init/init.c
	@mkdir -p $(dir $@)
	$(CC) $(INIT_CFLAGS) -c $< -o $@

$(BUILDDIR)/init.elf: $(BUILDDIR)/init.o init.ld
	$(LD) -T init.ld -nostdlib -o $@ $<

# Userspace programs to inject into disk image
USERSPACE_BINS = $(BUILDDIR)/init.elf

# ── Disk image ───────────────────────────────────────────────────────

$(BUILDDIR)/disk.img: $(USERSPACE_BINS)
	@mkdir -p $(BUILDDIR)
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	@python3 scripts/mkfat32img.py $@ 16 $(BUILDDIR)/init.elf /mnt/init.elf

# ── Run targets ───────────────────────────────────────────────────────

run: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	sudo qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std \
		-display cocoa -k en-us \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev vmnet-shared,id=net0 -device e1000,netdev=net0 ; \
	stty sane

run-virtio: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
		-no-reboot

# ── Test build (separate output dir, compiled with -DTEST_MODE) ──────

BUILDDIR_TEST = build_test
TEST_CFLAGS   = $(CFLAGS) -DTEST_MODE

# Optional: skip ATA disk tests (faster in TCG, no disk needed)
ifneq ($(SKIP_DISK_TESTS),)
TEST_CFLAGS += -DSKIP_DISK_TESTS
endif

C_TEST_SRCS  = $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) $(DOOM_SRCS) src/test/test.c
ASM_TEST_SRCS = $(ASM_SRCS)

C_TEST_OBJS  = $(patsubst src/%.c,$(BUILDDIR_TEST)/%.o,$(C_TEST_SRCS))
ASM_TEST_OBJS = $(patsubst src/%.asm,$(BUILDDIR_TEST)/%.o,$(ASM_TEST_SRCS))
TEST_OBJS    = $(ASM_TEST_OBJS) $(C_TEST_OBJS)

# Test build variant config
BUILD_CONFIG_GZ_H_TEST = $(BUILDDIR_TEST)/build_config_gz.h
$(BUILDDIR_TEST)/kernel/config_gz.o: CFLAGS += -I$(BUILDDIR_TEST)
$(BUILD_CONFIG_GZ_H_TEST): $(BUILD_CONFIG_GZ)
	@mkdir -p $(dir $@)
	@echo '/* Auto-generated — do not edit. */' > $@
	@echo '#ifndef BUILD_CONFIG_GZ_H' >> $@
	@echo '#define BUILD_CONFIG_GZ_H' >> $@
	xxd -i -n build_config_gz < $< >> $@
	@echo '#endif /* BUILD_CONFIG_GZ_H */' >> $@

$(BUILDDIR_TEST)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) -c $< -o $@

$(BUILDDIR_TEST)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR_TEST)/kernel.elf: check-app-boundary $(TEST_OBJS)
	@mkdir -p $(BUILDDIR_TEST)
	$(LD) $(LDFLAGS) -o $@ $(TEST_OBJS) -L/usr/lib/gcc/x86_64-linux-gnu/13 -lgcc

# config_gz.o depends on the auto-generated header for test build too
$(BUILDDIR_TEST)/kernel/config_gz.o: $(BUILD_CONFIG_GZ_H_TEST)

$(BUILDDIR_TEST)/kernel.bin: $(BUILDDIR_TEST)/kernel.elf
	cp $< $@

# Build the test kernel binary (parallel via recursive make)
test-kernel:
	$(MAKE) -j$(NPROCS) $(BUILDDIR_TEST)/kernel.bin

# Run headless QEMU on serial TCP 4444 for manual inspection of test kernel
test-serial: $(BUILDDIR_TEST)/kernel.bin $(BUILDDIR)/disk.img
	qemu-system-x86_64 -kernel $(BUILDDIR_TEST)/kernel.bin -m 256M \
		-serial tcp::4444,server,nowait \
		-vga none -display none \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device e1000,netdev=net0 \
		-no-reboot

# Run E2E tests: build test kernel in parallel, then boot in QEMU
test: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) test-kernel
	@chmod +x tests/run_tests.sh
	@./tests/run_tests.sh $(BUILDDIR_TEST)/kernel.bin $(BUILDDIR)/disk.img

# ── Check target: full build with -Werror + run all tests ──────────────
CHECK_CFLAGS = $(CFLAGS) -Werror
BUILDDIR_CHECK = build_check

C_CHECK_SRCS  = $(C_SRCS) $(CMD_SRCS) $(COMPILER_SRCS) $(GUI_SRCS) $(DOOM_SRCS) src/test/test.c
ASM_CHECK_SRCS = $(ASM_SRCS)

C_CHECK_OBJS  = $(patsubst src/%.c,$(BUILDDIR_CHECK)/%.o,$(C_CHECK_SRCS))
ASM_CHECK_OBJS = $(patsubst src/%.asm,$(BUILDDIR_CHECK)/%.o,$(ASM_CHECK_SRCS))
CHECK_OBJS    = $(ASM_CHECK_OBJS) $(C_CHECK_OBJS)

$(BUILDDIR_CHECK)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHECK_CFLAGS) -c $< -o $@

$(BUILDDIR_CHECK)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILDDIR_CHECK)/kernel.elf: check-app-boundary $(CHECK_OBJS)
	@mkdir -p $(BUILDDIR_CHECK)
	$(LD) $(LDFLAGS) -o $@ $(CHECK_OBJS) -L/usr/lib/gcc/x86_64-linux-gnu/13 -lgcc

$(BUILDDIR_CHECK)/kernel.bin: $(BUILDDIR_CHECK)/kernel.elf
	cp $< $@

check: $(BUILDDIR)/disk.img unit-test
	$(MAKE) -j$(NPROCS) $(BUILDDIR_CHECK)/kernel.bin
	@chmod +x tests/run_tests.sh
	@./tests/run_tests.sh $(BUILDDIR_CHECK)/kernel.bin $(BUILDDIR)/disk.img
	@echo ""
	@echo "=== Build-time tests passed, running E2E smoke test ==="
	$(MAKE) e2e-smoke

# Clean the check build artifacts
check-clean:
	rm -rf $(BUILDDIR_CHECK)

# ── Host-side unit tests (compiled with host gcc, no kernel deps) ───
unit-test:
	@echo "=== Host-side unit tests ==="
	$(MAKE) -C tests/host_libc all
	$(MAKE) -C tests/unit all

# Full clean rebuild + test
test-clean: clean
	$(MAKE) test

# E2E tests: boot normal kernel in QEMU with user-mode networking + telnet hostfwd
e2e: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin
	@chmod +x tests/e2e.sh tests/e2e.py
	@./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# E2E smoke test: fast CI subset of e2e tests
e2e-smoke: $(BUILDDIR)/disk.img
	$(MAKE) -j$(NPROCS) $(BUILDDIR)/kernel.bin
	@chmod +x tests/e2e.sh tests/e2e.py
	@E2E_SMOKE=1 ./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# Fast run: build test-kernel and run tests in one invocation
run-test:
	$(MAKE) test

# Verify doom framebuffer (PCI BAR0) renders non-black pixels in QEMU -vga std
doom-test: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/doom_fb.sh
	@./tests/doom_fb.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# E2E with explicit telnet port (override default 2323)
e2e-port-%: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	@chmod +x tests/e2e.sh tests/e2e.py
	@E2E_PORT=$* ./tests/e2e.sh $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img

# ── Debug target ──────────────────────────────────────────────────────

debug: $(BUILDDIR)/kernel.bin $(BUILDDIR)/disk.img
	sudo qemu-system-x86_64 -kernel $(BUILDDIR)/kernel.bin -m 256M -serial stdio -vga std -s -S \
		-drive file=$(BUILDDIR)/disk.img,format=raw,if=ide \
		-netdev vmnet-shared,id=net0 -device e1000,netdev=net0

# ── Clean targets ─────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR) $(BUILDDIR_TEST)

# Clean everything including ccache statistics
clean-all: clean
	@if command -v ccache >/dev/null 2>&1; then \
		ccache --clear 2>/dev/null; \
		ccache --zero-stats 2>/dev/null; \
		echo "ccache stats cleared."; \
	fi

# ── Format: run clang-format on all .c and .h files ───────────────────

format:
	@if command -v clang-format >/dev/null 2>&1; then \
		find src/ -type f \( -name '*.c' -o -name '*.h' \) -exec clang-format -i -style=file {} +; \
		echo "Formatted all .c and .h files in src/."; \
	else \
		echo "clang-format not found. Install it (e.g., apt install clang-format) and try again."; \
		exit 1; \
	fi

# ── Format check: verify code matches clang-format style ────────────
# Uses git-clang-format to check only lines that differ from the base
# ref (default: HEAD~1). Exits with 1 if any formatting issues found.

format-check:
	@if command -v git-clang-format >/dev/null 2>&1; then \
		BASE_REF="$${FORMAT_BASE_REF:-HEAD~1}"; \
		echo "Checking code formatting against $${BASE_REF}..."; \
		git clang-format --diff "$${BASE_REF}" 2>/dev/null > /tmp/format_diff.$$$$; \
		if [ -s /tmp/format_diff.$$$$ ]; then \
			echo "❌ Code formatting issues found:"; \
			cat /tmp/format_diff.$$$$; \
			rm -f /tmp/format_diff.$$$$; \
			exit 1; \
		else \
			echo "✅ Code formatting is clean."; \
			rm -f /tmp/format_diff.$$$$; \
		fi \
	else \
		echo "git-clang-format not found. Install it (e.g., apt install clang-format) and try again."; \
		exit 1; \
	fi

# ── Lint: run cppcheck on all C sources ───────────────────────────────

lint:
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all \
		  --suppress=unusedFunction \
		  --suppress=constVariablePointer \
		  --suppress=constParameterPointer \
		  --suppress=variableScope \
		  --suppress=unreadVariable \
		  --suppress=arrayIndexThenCheck \
		  --suppress=redundantInitialization \
		  --suppress=knownConditionTrueFalse \
		  --suppress=badBitmaskCheck \
		  --suppress=unusedStructMember \
		  --suppress=constVariable \
		  --suppress=shadowVariable \
		  --suppress=redundantAssignment \
		  --suppress=oppositeInnerCondition \
		  --suppress=zerodivcond \
		  --suppress=staticStringCompare \
		  --suppress=unsignedLessThanZero \
		  --suppress=arrayIndexOutOfBoundsCond \
		  --suppress=constParameterCallback \
		  --suppress=clarifyCondition \
		  --suppress=redundantCondition \
		  --suppress=duplicateValueTernary \
		  --suppress=useStandardLibrary \
		  --suppress=checkersReport \
		  --suppress=checkLevelNormal \
		  --std=c17 \
		  -Isrc/include -Isrc/gui -Isrc/doom \
		  --inline-suppr \
		  --error-exitcode=1 \
		  src/; \
	else \
		echo "cppcheck not found. Install it (e.g., apt install cppcheck) and try again."; \
		exit 1; \
	fi

# ── Dependencies ──────────────────────────────────────────────────────

deps:
	brew install x86_64-elf-gcc nasm qemu xorriso

# ── Build info & stats ─────────────────────────────────────────────────

build-info:
	@echo "=== Build Info ==="
	@echo "Kernel size: $$(ls -lh $(BUILDDIR)/kernel.bin 2>/dev/null | awk '{print $$5}')"
	@echo "Object count: $$(find $(BUILDDIR) -name '*.o' 2>/dev/null | wc -l)"
	@echo "Source files: $$(find src -name '*.c' -o -name '*.asm' -o -name '*.h' | wc -l)"
	@echo "Total LOC: $$(find src -name '*.c' -o -name '*.asm' -o -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}')"

# ── Source code line count ─────────────────────────────────────────────

count:
	@echo "=== Source Code Statistics ==="
	@echo "  C sources:  $$(find src -name '*.c' | wc -l) files, $$(find src -name '*.c' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"
	@echo "  Assembly:   $$(find src -name '*.asm' | wc -l) files, $$(find src -name '*.asm' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"
	@echo "  Headers:    $$(find src -name '*.h' | wc -l) files, $$(find src -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"
	@echo "  Tests:      $$(find tests -name '*.c' -o -name '*.sh' -o -name '*.py' | wc -l) files"

# ── ccache statistics ──────────────────────────────────────────────────

ccache-stats:
	@if command -v ccache >/dev/null 2>&1; then \
		ccache --show-stats; \
	else \
		echo "ccache not installed."; \
	fi
