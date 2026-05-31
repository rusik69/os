#include "rtc.h"
#include "io.h"
#include "idt.h"
#include "apic.h"
#include "pic.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS   0x00
#define RTC_MINUTES   0x02
#define RTC_HOURS     0x04
#define RTC_DAY       0x07
#define RTC_MONTH     0x08
#define RTC_YEAR      0x09
#define RTC_STATUS_A  0x0A
#define RTC_STATUS_B  0x0B
#define RTC_STATUS_C  0x0C

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static int is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* Days in each month (non-leap) */
static const int days_in_mon[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

uint64_t rtc_to_epoch(const struct rtc_time *t) {
    /* Compute days since 2000-01-01 (Unix time for 2000 = 946684800) */
    int y = (int)t->year;
    int m = (int)t->month;
    int d = (int)t->day;
    int h = (int)t->hour;
    int min = (int)t->minute;
    int s = (int)t->second;

    /* Count days from 2000 to the target year */
    uint64_t days = 0;
    for (int yr = 2000; yr < y; yr++)
        days += is_leap(yr) ? 366 : 365;

    /* Days in current year up to current month */
    for (int mo = 1; mo < m; mo++) {
        days += days_in_mon[mo - 1];
        if (mo == 2 && is_leap(y)) days++;
    }
    days += (d - 1);

    /* Convert to seconds (2000 epoch) then add Unix base offset */
    uint64_t epoch = days * 86400ULL + h * 3600ULL + min * 60ULL + s;
    return epoch + 946684800ULL;  /* Unix epoch for 2000-01-01 */
}

/* ── Boot epoch (Unix wall clock seconds at boot time) ────────────── */

static uint64_t boot_epoch_seconds = 0;

uint64_t rtc_get_epoch(void) {
    return boot_epoch_seconds;
}

void rtc_set_epoch(uint64_t s) {
    boot_epoch_seconds = s;
}

/* ── RTC read helpers ─────────────────────────────────────────────── */

static int is_updating(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

static void rtc_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    /* Read status C to clear the interrupt */
    cmos_read(RTC_STATUS_C);
    irq_ack(8);
}

void rtc_init(void) {
    /* Enable RTC IRQ8 by unmasking it in PIC */
    /* Register our handler for IRQ8 (vector 40) */
    idt_register_handler(40, rtc_irq_handler);

    if (apic_is_init_complete()) {
        ioapic_unmask_irq(8);
    }
    pic_unmask(8);

    /* Enable Update-Ended interrupt (bit 4 of status B) */
    outb(CMOS_ADDR, 0x8B);   /* disable NMI, select status B */
    io_wait();
    uint8_t regb = inb(CMOS_DATA);
    outb(CMOS_ADDR, 0x8B);
    io_wait();
    outb(CMOS_DATA, regb | 0x10);
    io_wait();

    /* Read status C once to clear any pending interrupt */
    outb(CMOS_ADDR, 0x0C);
    io_wait();
    inb(CMOS_DATA);

    /* Capture boot epoch from RTC so clock_gettime(CLOCK_REALTIME) works */
    struct rtc_time boot_time;
    rtc_get_time(&boot_time);
    boot_epoch_seconds = rtc_to_epoch(&boot_time);
}

void rtc_get_time(struct rtc_time *t) {
    uint8_t sec, min, hr, day, mon, yr;
    uint8_t last_sec, last_min, last_hr, last_day, last_mon, last_yr;

    /* Wait for RTC to be stable - read twice until consistent */
    do {
        while (is_updating());
        sec = cmos_read(RTC_SECONDS);
        min = cmos_read(RTC_MINUTES);
        hr  = cmos_read(RTC_HOURS);
        day = cmos_read(RTC_DAY);
        mon = cmos_read(RTC_MONTH);
        yr  = cmos_read(RTC_YEAR);

        while (is_updating());
        last_sec = cmos_read(RTC_SECONDS);
        last_min = cmos_read(RTC_MINUTES);
        last_hr  = cmos_read(RTC_HOURS);
        last_day = cmos_read(RTC_DAY);
        last_mon = cmos_read(RTC_MONTH);
        last_yr  = cmos_read(RTC_YEAR);
    } while (sec != last_sec || min != last_min || hr != last_hr ||
             day != last_day || mon != last_mon || yr != last_yr);

    /* Check if BCD mode (bit 2 of status B clear = BCD) */
    uint8_t regb = cmos_read(RTC_STATUS_B);
    if (!(regb & 0x04)) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hr  = bcd_to_bin(hr & 0x7F) | (hr & 0x80); /* preserve PM bit */
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        yr  = bcd_to_bin(yr);
    }

    /* 12/24 hour: if 12-hour mode (bit 1 of regb clear) and PM bit set */
    if (!(regb & 0x02) && (hr & 0x80)) {
        hr = ((hr & 0x7F) + 12) % 24;
    }

    t->second = sec;
    t->minute = min;
    t->hour   = hr;
    t->day    = day;
    t->month  = mon;
    /* Year: assume 2000s. RTC gives 2-digit year. */
    t->year   = (yr < 70) ? (2000 + yr) : (1900 + yr);
}
