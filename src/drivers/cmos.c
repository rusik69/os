#include "cmos.h"
#include "io.h"

uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

void cmos_write(uint8_t reg, uint8_t val) {
    outb(CMOS_ADDR, reg);
    io_wait();
    outb(CMOS_DATA, val);
    io_wait();
}

uint8_t cmos_nvram_read(uint8_t offset) {
    if (offset > (CMOS_NVRAM_END - CMOS_NVRAM_START))
        return 0;
    return cmos_read(CMOS_NVRAM_START + offset);
}

void cmos_nvram_write(uint8_t offset, uint8_t val) {
    if (offset > (CMOS_NVRAM_END - CMOS_NVRAM_START))
        return;
    cmos_write(CMOS_NVRAM_START + offset, val);
}

void cmos_init(void) {
    /* no-op: CMOS is always accessible */
}
