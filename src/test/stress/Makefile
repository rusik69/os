# src/test/stress/Makefile — Stress test programs for Hermes OS
#
# Builds userspace stress-test ELFs for the OS filesystem.
# Uses the same toolchain as the userspace build.

CROSS_COMPILE ?= x86_64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
LD := $(CROSS_COMPILE)ld

LIBC_DIR = ../../../userspace/libc
LIBC_A = $(LIBC_DIR)/libc.a
LIBC_USER_LD = $(LIBC_DIR)/user.ld

CFLAGS = -ffreestanding -nostdlib -nostdinc -fno-builtin -mno-red-zone \
         -I$(LIBC_DIR)/include -Wall -Wextra -O2 -g -MMD -MP
LDFLAGS = -T $(LIBC_USER_LD) -nostdlib -z max-page-size=0x1000 -z noexecstack

STRESS_ELFS = stress_cpu.elf stress_memory.elf stress_disk.elf stress_runner.elf

.PHONY: all clean

all: $(STRESS_ELFS)

stress_cpu.elf: stress_cpu.o $(LIBC_A)
	$(LD) $(LDFLAGS) stress_cpu.o $(LIBC_A) -o $@

stress_memory.elf: stress_memory.o $(LIBC_A)
	$(LD) $(LDFLAGS) stress_memory.o $(LIBC_A) -o $@

stress_disk.elf: stress_disk.o $(LIBC_A)
	$(LD) $(LDFLAGS) stress_disk.o $(LIBC_A) -o $@

stress_runner.elf: stress_runner.o $(LIBC_A)
	$(LD) $(LDFLAGS) stress_runner.o $(LIBC_A) -o $@

stress_cpu.o: stress_cpu.c
	$(CC) $(CFLAGS) -c $< -o $@

stress_memory.o: stress_memory.c
	$(CC) $(CFLAGS) -c $< -o $@

stress_disk.o: stress_disk.c
	$(CC) $(CFLAGS) -c $< -o $@

stress_runner.o: stress_runner.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf *.d
