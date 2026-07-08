#include "cmos.h"
#include "io.h"
#include "rtc.h"
#include "errno.h"

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
#include "module.h"
module_init(cmos_init);

/* ── Get time from CMOS RTC ─────────────────────────── */
static int cmos_get_time(void *time)
{
    struct rtc_time *t = (struct rtc_time *)time;
    if (!t)
        return -EINVAL;

    /* Wait for update-in-progress to clear */
    int timeout = 10000;
    while ((cmos_read(0x0A) & 0x80) && --timeout > 0)
        io_wait();

    t->second = cmos_read(0x00);
    t->minute = cmos_read(0x02);
    t->hour   = cmos_read(0x04);
    t->day    = cmos_read(0x07);
    t->month  = cmos_read(0x08);
    t->year   = (uint16_t)cmos_read(0x09) + 2000;

    /* Convert BCD to binary if needed */
    uint8_t status_b = cmos_read(0x0B);
    if (!(status_b & 0x04)) {
        t->second = (uint8_t)((t->second & 0x0F) + ((t->second / 16) * 10));
        t->minute = (uint8_t)((t->minute & 0x0F) + ((t->minute / 16) * 10));
        t->hour   = (uint8_t)((t->hour & 0x0F) + ((t->hour / 16) * 10));
        t->day    = (uint8_t)((t->day & 0x0F) + ((t->day / 16) * 10));
        t->month  = (uint8_t)((t->month & 0x0F) + ((t->month / 16) * 10));
        t->year   = (uint16_t)((t->year & 0x0F) + ((t->year / 16) * 10) + 2000);
    }

    return 0;
}

/* ── Set time in CMOS RTC ───────────────────────────── */
static int cmos_set_time(const void *time)
{
    const struct rtc_time *t = (const struct rtc_time *)time;
    if (!t)
        return -EINVAL;

    uint8_t bcd_sec = (t->second / 10) << 4 | (t->second % 10);
    uint8_t bcd_min = (t->minute / 10) << 4 | (t->minute % 10);
    uint8_t bcd_hr  = (t->hour / 10) << 4 | (t->hour % 10);
    uint8_t bcd_day = (t->day / 10) << 4 | (t->day % 10);
    uint8_t bcd_mon = (t->month / 10) << 4 | (t->month % 10);
    uint16_t year_short = t->year % 100;
    uint8_t bcd_yr  = (uint8_t)((year_short / 10) << 4 | (year_short % 10));

    /* Disable NMI and set SET bit to prevent updates during write */
    uint8_t prev = cmos_read(0x0B);
    cmos_write(0x0B, prev | 0x80);

    cmos_write(0x00, bcd_sec);
    cmos_write(0x02, bcd_min);
    cmos_write(0x04, bcd_hr);
    cmos_write(0x06, 0);  /* day of week — skip */
    cmos_write(0x07, bcd_day);
    cmos_write(0x08, bcd_mon);
    cmos_write(0x09, bcd_yr);
    cmos_write(0x32, (uint8_t)((t->year / 100) % 100));

    /* Clear SET bit */
    cmos_write(0x0B, prev & ~0x80);

    return 0;
}
