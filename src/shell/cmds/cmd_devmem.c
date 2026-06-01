/* cmd_devmem.c — read/write physical memory via libc IO functions */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_devmem(int argc, char **argv) {
    int write_mode = 0;
    uint64_t addr = 0;
    uint64_t val = 0;
    int width = 32;

    if (argc < 2) {
        kprintf("Usage: devmem <address> [width] [value]\n");
        kprintf("  Read/write physical memory at ADDRESS\n");
        kprintf("  width: 8, 16, or 32 (default 32)\n");
        kprintf("  value: if provided, write instead of read\n");
        return 1;
    }

    addr = strtoul(argv[1], NULL, 0);
    if (argc >= 3) {
        width = atoi(argv[2]);
        if (width != 8 && width != 16 && width != 32) {
            kprintf("devmem: width must be 8, 16, or 32\n");
            return 1;
        }
    }
    if (argc >= 4) {
        write_mode = 1;
        val = strtoul(argv[3], NULL, 0);
    }

    if (write_mode) {
        if (width == 8) {
            /* Use physical memory mapping via address in VMA offset range */
            uint8_t *ptr = (uint8_t *)(addr + 0xFFFF800000000000ULL);
            *ptr = (uint8_t)(val & 0xFF);
            kprintf("devmem: wrote 0x%02llX to 0x%llX\n", (unsigned long long)(val & 0xFF), (unsigned long long)addr);
        } else if (width == 16) {
            uint16_t *ptr = (uint16_t *)(addr + 0xFFFF800000000000ULL);
            *ptr = (uint16_t)(val & 0xFFFF);
            kprintf("devmem: wrote 0x%04llX to 0x%llX\n", (unsigned long long)(val & 0xFFFF), (unsigned long long)addr);
        } else {
            uint32_t *ptr = (uint32_t *)(addr + 0xFFFF800000000000ULL);
            *ptr = (uint32_t)(val & 0xFFFFFFFF);
            kprintf("devmem: wrote 0x%08llX to 0x%llX\n", (unsigned long long)(val & 0xFFFFFFFF), (unsigned long long)addr);
        }
    } else {
        if (width == 8) {
            uint8_t *ptr = (uint8_t *)(addr + 0xFFFF800000000000ULL);
            kprintf("0x%02X\n", *ptr);
        } else if (width == 16) {
            uint16_t *ptr = (uint16_t *)(addr + 0xFFFF800000000000ULL);
            kprintf("0x%04X\n", *ptr);
        } else {
            uint32_t *ptr = (uint32_t *)(addr + 0xFFFF800000000000ULL);
            kprintf("0x%08X\n", *ptr);
        }
    }
    return 0;
}
