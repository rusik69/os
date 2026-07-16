#include "rtc.h"
#include "io.h"
#include "idt.h"
#include "apic.h"
#include "pic.h"
#include "printf.h"
#include "string.h"
#include "waitqueue.h"
#include "devfs.h"
#include "signal.h"

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
#define RTC_STATUS_D  0x0D

/* Alarm registers */
#define RTC_ALRM_SEC  0x01
#define RTC_ALRM_MIN  0x03
#define RTC_ALRM_HRS  0x05
#define RTC_ALRM_DAY  0x06

/* Status register B bits */
#define RTC_B_UPD_END  (1U << 4)  /* Update-ended interrupt enable */
#define RTC_B_PER_INT  (1U << 6)  /* Periodic interrupt enable */
#define RTC_B_ALRM_INT (1U << 5)  /* Alarm interrupt enable */

/* Status register A bits */
#define RTC_A_RATE_MASK 0x0F     /* Periodic rate select */
#define RTC_A_UIP       0x80     /* Update in progress */

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static void cmos_write(uint8_t reg, uint8_t val) {
    outb(CMOS_ADDR, reg);
    io_wait();
    outb(CMOS_DATA, val);
    io_wait();
}

static int is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

/* Days in each month (non-leap) */
static const int days_in_mon[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

uint64_t rtc_to_epoch(const struct rtc_time *t) {
    int y = (int)t->year;
    int m = (int)t->month;
    int d = (int)t->day;
    int h = (int)t->hour;
    int min = (int)t->minute;
    int s = (int)t->second;

    uint64_t days = 0;
    for (int yr = 2000; yr < y; yr++)
        days += is_leap(yr) ? 366 : 365;

    for (int mo = 1; mo < m; mo++) {
        days += days_in_mon[mo - 1];
        if (mo == 2 && is_leap(y)) days++;
    }
    days += (d - 1);

    uint64_t epoch = days * 86400ULL + h * 3600ULL + min * 60ULL + s;
    return epoch + 946684800ULL;
}

/* ── Boot epoch ───────────────────────────────────────────────────── */

static uint64_t boot_epoch_seconds = 0;

uint64_t rtc_get_epoch(void) {
    return boot_epoch_seconds;
}

void rtc_set_epoch(uint64_t s) {
    boot_epoch_seconds = s;
}

/* ── Periodic interrupt state ─────────────────────────────────────── */

static volatile uint64_t g_periodic_ticks = 0;
static volatile int g_periodic_enabled = 0;

/* ── /dev/rtc userspace interface state ───────────────────────── */

/* Wait queue for processes blocking on /dev/rtc read */
static struct wait_queue g_rtc_dev_wq;
/* If non-zero, deliver SIGALRM to this PID on each periodic tick */
static int g_rtc_sigalrm_pid = 0;

/* ── Alarm state ──────────────────────────────────────────────────── */

static volatile int g_alarm_fired = 0;
static uint64_t g_wakealarm_epoch = 0;  /* alarm set time in epoch seconds (0 = disabled) */

/* ── Forward declarations ────────────────────────────────────────── */

/* Initialize the /dev/rtc devfs device (defined later in this file) */
static void rtc_devfs_init(void);

/* ── RTC read helpers ─────────────────────────────────────────────── */

static int is_updating(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t)((bcd & 0x0F) + ((bcd >> 4) * 10));
}

static uint8_t bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}

static void rtc_irq_handler(struct interrupt_frame *frame) {
    (void)frame;

    /* Read status C to clear the interrupt and determine source */
    uint8_t status_c = cmos_read(RTC_STATUS_C);

    /* Periodic interrupt */
    if (g_periodic_enabled && (status_c & 0x40)) {
        g_periodic_ticks++;

        /* Wake any processes blocked on /dev/rtc read */
        wait_queue_wake_all(&g_rtc_dev_wq);

        /* Deliver SIGALRM to registered process if any */
        if (g_rtc_sigalrm_pid > 0) {
            signal_send((uint32_t)g_rtc_sigalrm_pid, SIGALRM);
        }
    }

    /* Alarm interrupt */
    if (status_c & 0x20) {
        g_alarm_fired = 1;
    }

    irq_ack(8);
}

void __init rtc_init(void) {
    idt_register_handler_named(40, rtc_irq_handler, "cmos_rtc");

    /* Initialize wait queue for /dev/rtc blocking reads */
    wait_queue_init(&g_rtc_dev_wq);

    if (apic_is_init_complete()) {
        ioapic_unmask_irq(8);
    }
    pic_unmask(8);

    /* Enable Update-Ended interrupt (bit 4 of status B) */
    uint8_t regb = cmos_read(RTC_STATUS_B);
    cmos_write(RTC_STATUS_B, regb | RTC_B_UPD_END);

    /* Read status C once to clear any pending interrupt */
    cmos_read(RTC_STATUS_C);

    /* Capture boot epoch from RTC */
    struct rtc_time boot_time;
    rtc_get_time(&boot_time);
    boot_epoch_seconds = rtc_to_epoch(&boot_time);

    /* Register /dev/rtc device for userspace access */
    rtc_devfs_init();
}

/* ── Periodic interrupt API ────────────────────────────────────────── */

/* Convert rate in Hz to the 4-bit RTC rate select code.
   RTC uses divider 32768 >> (rate_select - 1) for rate_select >= 3 */
static int hz_to_rate_select(int rate_hz) {
    if (rate_hz <= 0) return -1;
    /* Rate = 32768 >> (rs - 1), so rs = 16 - log2(32768/rate) */
    int div = 32768 / rate_hz;
    int rs = 16;
    while (div > 1 && rs > 2) {
        div >>= 1;
        rs--;
    }
    if (rs < 3 || rs > 15) return -1;
    return rs;
}

int rtc_set_periodic(int enable, int rate_hz) {
    if (enable) {
        int rs = hz_to_rate_select(rate_hz);
        if (rs < 0) return -1;

        /* Set rate in Status Register A */
        uint8_t rega = cmos_read(RTC_STATUS_A);
        rega = (rega & ~RTC_A_RATE_MASK) | (uint8_t)(rs & 0x0F);
        cmos_write(RTC_STATUS_A, rega);

        /* Enable periodic interrupt in Status Register B */
        uint8_t regb = cmos_read(RTC_STATUS_B);
        cmos_write(RTC_STATUS_B, regb | RTC_B_PER_INT);

        g_periodic_ticks = 0;
        g_periodic_enabled = 1;
    } else {
        /* Disable periodic interrupt */
        uint8_t regb = cmos_read(RTC_STATUS_B);
        cmos_write(RTC_STATUS_B, (uint8_t)(regb & ~RTC_B_PER_INT));
        g_periodic_enabled = 0;
    }

    return 0;
}

int rtc_wait_ticks(uint32_t ticks) {
    if (!g_periodic_enabled) return -1;

    uint64_t target = g_periodic_ticks + ticks;
    while (g_periodic_ticks < target) {
        __asm__ volatile("pause; pause; pause");
    }

    return 0;
}

/* Return current RTC periodic tick count */
uint64_t rtc_get_ticks(void) {
    return g_periodic_ticks;
}

/* ── Alarm API ─────────────────────────────────────────────────────── */

int rtc_set_alarm(const struct rtc_time *t) {
    if (!t) return -1;

    /* Write alarm registers */
    cmos_write(RTC_ALRM_SEC, bin_to_bcd(t->second));
    cmos_write(RTC_ALRM_MIN, bin_to_bcd(t->minute));
    cmos_write(RTC_ALRM_HRS, bin_to_bcd(t->hour));

    /* Convert the alarm time to epoch seconds and store it */
    g_wakealarm_epoch = rtc_to_epoch(t);

    return 0;
}

/* Set alarm from epoch seconds (userspace wakealarm interface).
 * Returns 0 on success, -1 if time is in the past (ignored). */
int rtc_set_alarm_epoch(uint64_t epoch_sec) {
    if (epoch_sec == 0) {
        /* Disable alarm */
        rtc_alarm_enable(0);
        g_wakealarm_epoch = 0;
        return 0;
    }

    /* Convert epoch seconds to rtc_time */
    uint64_t remaining = epoch_sec;
    struct rtc_time t;
    memset(&t, 0, sizeof(t));
    t.year = 2000;

    /* Subtract years */
    for (;;) {
        int days = is_leap((int)t.year) ? 366 : 365;
        uint64_t secs = (uint64_t)days * 86400ULL;
        if (remaining < secs) break;
        remaining -= secs;
        t.year++;
    }

    /* Subtract months */
    for (int mo = 1; mo <= 12; mo++) {
        int days = days_in_mon[mo - 1];
        if (mo == 2 && is_leap((int)t.year)) days++;
        uint64_t secs = (uint64_t)days * 86400ULL;
        if (remaining < secs) break;
        remaining -= secs;
        t.month = (uint8_t)mo;
    }
    t.month++; /* month is 1-indexed */
    t.day = (uint8_t)(remaining / 86400ULL) + 1;
    remaining %= 86400ULL;
    t.hour = (uint8_t)(remaining / 3600ULL);
    remaining %= 3600ULL;
    t.minute = (uint8_t)(remaining / 60ULL);
    t.second = (uint8_t)(remaining % 60ULL);

    g_wakealarm_epoch = epoch_sec;
    return rtc_set_alarm(&t);
}

int rtc_alarm_enable(int enable) {
    uint8_t regb = cmos_read(RTC_STATUS_B);
    if (enable) {
        cmos_write(RTC_STATUS_B, regb | RTC_B_ALRM_INT);
    } else {
        cmos_write(RTC_STATUS_B, (uint8_t)(regb & ~RTC_B_ALRM_INT));
    }
    return 0;
}

int rtc_alarm_fired(void) {
    return g_alarm_fired ? 1 : 0;
}

void rtc_alarm_clear(void) {
    g_alarm_fired = 0;
    /* Read status C to clear pending alarm flag */
    cmos_read(RTC_STATUS_C);
}

/* ── Time reading ──────────────────────────────────────────────────── */

void rtc_get_time(struct rtc_time *t) {
    uint8_t sec, min, hr, day, mon, yr;
    uint8_t last_sec, last_min, last_hr, last_day, last_mon, last_yr;

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

    uint8_t regb = cmos_read(RTC_STATUS_B);
    if (!(regb & 0x04)) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hr  = bcd_to_bin(hr & 0x7F) | (hr & 0x80);
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        yr  = bcd_to_bin(yr);
    }

    if (!(regb & 0x02)) {
        hr = (uint8_t)(((hr & 0x7F) % 12) + ((hr & 0x80) ? 12 : 0));
    }

    t->second = sec;
    t->minute = min;
    t->hour   = hr;
    t->day    = day;
    t->month  = mon;
    t->year   = (yr < 70) ? (2000 + yr) : (1900 + yr);
}

/* ── /dev/rtc userspace device interface ──────────────────────────── */

/*
 * rtc_dev_read — Blocking read from /dev/rtc
 *
 * Returns the current periodic tick count as a uint64_t (8 bytes).
 * If periodic interrupts are not enabled, returns error.
 * If no tick has occurred since last read, blocks until the next
 * periodic interrupt fires.
 */
static int rtc_dev_read(void *priv, void *buf, uint32_t max_size,
                         uint32_t *out_size)
{
    (void)priv;

    if (!g_periodic_enabled)
        return -1;

    /* Wait for at least one tick to have occurred */
    uint64_t start_ticks = g_periodic_ticks;
    while (g_periodic_ticks == start_ticks) {
        wait_queue_sleep(&g_rtc_dev_wq);
        /* Re-check after wake — could be spurious or alarm interrupt */
    }

    /* Return the current tick count */
    uint64_t ticks = g_periodic_ticks;
    uint32_t copy = (max_size < sizeof(ticks)) ? max_size : (uint32_t)sizeof(ticks);
    memcpy(buf, &ticks, copy);
    if (out_size)
        *out_size = copy;
    return 0;
}

/*
 * rtc_dev_write — Write to /dev/rtc to configure periodic rate
 *
 * Accepted formats:
 *   "rate_hz=<n>\n"  — set periodic interrupt rate in Hz
 *   "sigalrm_pid=<n>\n" — set PID to deliver SIGALRM to (0 to disable)
 *   "0\n"  or "disable\n" — disable periodic interrupts
 *
 * Returns 0 on success, -1 on parse error.
 */
static int rtc_dev_write(void *priv, const void *data, uint32_t size)
{
    (void)priv;
    const char *s = (const char *)data;
    uint32_t remain = size;
    uint32_t i = 0;

    /* Skip leading whitespace */
    while (i < remain && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (i >= remain) return -1;

    /* Check for "disable" or "0" */
    if (s[i] == '0' || s[i] == 'd') {
        rtc_set_periodic(0, 0);
        g_rtc_sigalrm_pid = 0;
        return 0;
    }

    /* Parse "rate_hz=<number>" */
    if (remain - i >= 8 && strncmp(&s[i], "rate_hz=", 8) == 0) {
        i += 8;
        int rate = 0;
        while (i < remain && s[i] >= '0' && s[i] <= '9') {
            rate = rate * 10 + (int)(s[i] - '0');
            i++;
        }
        if (rate <= 0) return -1;
        /* Clamp to valid range: 2 to 8192 Hz (power of 2 divisors of 32768) */
        if (rate < 2) rate = 2;
        if (rate > 8192) rate = 8192;
        int ret = rtc_set_periodic(1, rate);
        if (ret != 0) return -1;
        return 0;
    }

    /* Parse "sigalrm_pid=<number>" */
    if (remain - i >= 12 && strncmp(&s[i], "sigalrm_pid=", 12) == 0) {
        i += 12;
        int pid = 0;
        while (i < remain && s[i] >= '0' && s[i] <= '9') {
            pid = pid * 10 + (int)(s[i] - '0');
            i++;
        }
        g_rtc_sigalrm_pid = pid;
        return 0;
    }

    /* Unrecognized command */
    return -1;
}

/* Initialize the /dev/rtc device node in devfs.
 * Called during rtc_init() to make the device available. */
static void rtc_devfs_init(void)
{
    wait_queue_init(&g_rtc_dev_wq);
    g_rtc_sigalrm_pid = 0;

    int ret = devfs_register_device("rtc", NULL,
                                     rtc_dev_read, rtc_dev_write);
    if (ret == 0) {
        kprintf("[OK] RTC: /dev/rtc registered (periodic 1-8192 Hz, SIGALRM delivery)\n");
    } else {
        kprintf("[RTC] WARN: failed to register /dev/rtc\n");
    }
}

#include "sysfs.h"

/* Read callback for /sys/class/rtc/rtc0/wakealarm.
 * Returns the current alarm time as epoch seconds (or "0\n" if disabled). */
static int wakealarm_read(char *buf, uint32_t max_size, void *priv) {
    (void)priv;
    if (max_size < 4) return -1;
    if (g_wakealarm_epoch == 0) {
        buf[0] = '0';
        buf[1] = '\n';
        buf[2] = '\0';
        return 2;
    }
    /* Format epoch seconds as decimal string */
    uint64_t val = g_wakealarm_epoch;
    char tmp[24];
    int len = 0;
    do {
        tmp[len++] = (char)('0' + (val % 10));
        val /= 10;
    } while (val > 0 && len < 22);
    /* Reverse into buffer */
    int out_len = len + 1; /* +1 for newline */
    if ((uint32_t)out_len >= max_size) return -1;
    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    buf[len] = '\n';
    buf[len + 1] = '\0';
    return out_len;
}

/* Write callback for /sys/class/rtc/rtc0/wakealarm.
 * Accepts an epoch-seconds value as a decimal string.
 * Writing "0" disables the alarm. */
static int wakealarm_write(const char *data, uint32_t size, void *priv) {
    (void)priv;
    if (size == 0) return -1;

    /* Parse the first whitespace-delimited token as a number */
    uint64_t val = 0;
    uint32_t i = 0;
    while (i < size && (data[i] == ' ' || data[i] == '\t'))
        i++;
    if (i >= size) return -1;

    /* Check for negative sign */
    int negative = 0;
    if (data[i] == '-') { negative = 1; i++; }
    else if (data[i] == '+') { i++; }

    /* Parse decimal digits */
    while (i < size && data[i] >= '0' && data[i] <= '9') {
        val = val * 10 + (uint64_t)(data[i] - '0');
        i++;
    }

    if (negative) val = 0; /* negative means disable */

    return rtc_set_alarm_epoch(val);
}

/* Initialize RTC sysfs interface.
 * Creates /sys/class/rtc/rtc0/wakealarm for wake-from-suspend. */
void __init rtc_sysfs_init(void) {
    /* Create /sys/class/rtc/ directory */
    if (sysfs_create_dir("/sys/class/rtc") < 0) {
        kprintf("[RTC] sysfs: failed to create /sys/class/rtc\n");
        return;
    }

    /* Create /sys/class/rtc/rtc0/ directory */
    if (sysfs_create_dir("/sys/class/rtc/rtc0") < 0) {
        kprintf("[RTC] sysfs: failed to create /sys/class/rtc/rtc0\n");
        return;
    }

    /* Create writable wakealarm file */
    if (sysfs_create_writable_file("/sys/class/rtc/rtc0/wakealarm",
                                    "0\n", NULL, wakealarm_read, wakealarm_write) < 0) {
        kprintf("[RTC] sysfs: failed to create wakealarm\n");
        return;
    }

    kprintf("[OK] RTC sysfs: /sys/class/rtc/rtc0/wakealarm\n");
}
#include "module.h"
module_init(rtc_init);

static int rtc_read_time(struct rtc_time *tm)
{
    if (!tm) return -EINVAL;
    /* Use existing rtc_get_time which reads the CMOS RTC correctly */
    rtc_get_time(tm);
    return 0;
}

static int rtc_set_time(const struct rtc_time *tm)
{
    (void)tm;
    return 0;
}

/* Standard Linux RTC ioctl commands (from linux/rtc.h) */
#define RTC_RD_TIME  0x80247009  /* _IOR('p', 0x09, struct rtc_time)  — read time */
#define RTC_SET_TIME 0x4024700A  /* _IOW('p', 0x0A, struct rtc_time)  — set time */

/* ── RTC ioctl handler ──────────────────────────────── */
static int rtc_ioctl(int cmd, void *arg)
{
    if (!arg)
        return -EINVAL;

    switch (cmd) {
    case RTC_RD_TIME: {
        struct rtc_time *rt = (struct rtc_time *)arg;
        rtc_get_time(rt);
        return 0;
    }
    case RTC_SET_TIME: {
        const struct rtc_time *rt = (const struct rtc_time *)arg;
        return rtc_set_time(rt);
    }
    default:
        return -ENOTTY;
    }
}
