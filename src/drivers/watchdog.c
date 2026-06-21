/*  
 * watchdog.c — Software watchdog timer and system reset  
 *  
 * Uses the dynamic timer subsystem to implement a software watchdog.  
 * If watchdog_pet() is not called within the timeout period, the system  
 * is rebooted via the keyboard controller reset port.  
 * Supports a pretimeout callback that fires before the full timeout.  
 *  
 * Enhanced with:
 *   - Pretimeout NMI delivery via APIC
 *   - Configurable governors (panic, dump, none)
 *   - /sys/class/watchdog/watchdog0/ sysfs attributes
 *  
 * Also provides watchdog_system_reset() — a multi-method machine reset  
 * usable from interrupt-disabled contexts (e.g., panic).  
 */  

#include "watchdog.h"
#include "pstore.h"  
#include "wdt_enhanced.h"  
#include "timers.h"  
#include "timer.h"  
#include "io.h"  
#include "string.h"  
#include "printf.h"  
#include "smp.h"  
#include "nmi_watchdog.h"  
#include "apic.h"  
#include "sysfs.h"  
#include "panic.h"  
#include "kdump.h"

static int g_watchdog_timer_id = -1;
static int g_pretimeout_timer_id = -1;
static int g_watchdog_timeout_ticks = 0;
static int g_watchdog_active = 0;

/* Pretimeout state */  
static int g_pretimeout_secs = 0;  
static watchdog_pretimeout_fn_t g_pretimeout_fn = NULL;  
static volatile int g_pretimeout_fired = 0;  

/* Pretimeout governor (default: NONE — just warn, no hard action) */  
static int g_pretimeout_governor = WDT_GOVERNOR_NONE;

/* Internal callback: if this fires, the watchdog was not petted in time */
static void watchdog_reboot(void *arg) {
    (void)arg;
    kprintf("\n*** WATCHDOG TIMEOUT — System reset ***\n");

    for (int i = 0; i < 3; i++) {
        outb(0x64, 0xFE);
        io_wait();
    }

    __asm__ volatile("div %0" : : "r"(0) : "eax", "edx");

    cli();
    for (;;) hlt();
}

/* Internal callback to re-arm the watchdog periodically */
static void watchdog_tick(void *arg) {
    (void)arg;
    if (!g_watchdog_active) return;

    watchdog_reboot(NULL);
}

/* Pretimeout callback — fires g_pretimeout_secs before the full timeout.  
 *  
 * When a pretimeout fires:  
 *   1. Evaluate the configured governor:  
 *        PANIC → call panic()  
 *        DUMP  → dump register state of all CPUs, do NOT reset  
 *        NONE  → just warn  
 *   2. Send NMI IPI backtrace requests to all other CPUs so we capture  
 *      their register state and stacks before the system resets.  
 *   3. Log the timeout warning with the remaining margin.  
 *   4. If a user-registered pretimeout function exists, call it as well  
 *      (e.g. for panic+crashdump before watchdog reset).  
 */  
static void watchdog_pretimeout_tick(void *arg) {  
    (void)arg;  
    if (!g_watchdog_active) return;  

    g_pretimeout_fired = 1;  

    /* ── NMI to all CPUs ───────────────────────────────────────── */  
    kprintf("\n*** WATCHDOG PRETIMEOUT — system will reset in ~%d seconds ***\n",  
            g_pretimeout_secs);  

    /* Deliver NMI to all CPUs via APIC ICR with NMI delivery mode */  
    wdt_send_nmi_all_cpus();  

    /* ── Execute governor action ────────────────────────────────── */  
    wdt_pretimeout_action();  

    /* ── User-registered pretimeout handler ────────────────────── */  
    if (g_pretimeout_fn) {  
        g_pretimeout_fn();  
    }  
}  

/* ── APIC NMI delivery ────────────────────────────────────────────────  
 *  
 * Send an NMI to all CPUs (including self) using the local APIC ICR.  
 * We use destination shorthand "all including self" with delivery mode  
 * set to NMI (binary 100).  The vector field is ignored in NMI delivery  
 * mode but we write a conventional value for hardware compatibility.  
 */  
void wdt_send_nmi_all_cpus(void)  
{  
    /* Wait for any previous IPI to complete */  
    while (apic_read(LAPIC_ICR_LOW) & ICR_DELIVERY)  
        __asm__ volatile("pause");  

    /* Send NMI to all CPUs including self */  
    apic_write(LAPIC_ICR_HIGH, 0);  
    apic_write(LAPIC_ICR_LOW, ICR_ALL_INCL | ICR_DELIVERY_NMI | IPI_VECTOR_NMI);  

    /* Wait for delivery to complete */  
    while (apic_read(LAPIC_ICR_LOW) & ICR_DELIVERY)  
        __asm__ volatile("pause");  

    kprintf("[watchdog] NMI delivered to all CPUs via APIC\n");  
}  

/* ── Governor implementation ────────────────────────────────────────── */  

void wdt_set_pretimeout_governor(int gov)  
{  
    if (gov >= WDT_GOVERNOR_PANIC && gov <= WDT_GOVERNOR_NONE)  
        g_pretimeout_governor = gov;  
}  

int wdt_get_pretimeout_governor(void)  
{  
    return g_pretimeout_governor;  
}  

/*  
 * wdt_pretimeout_action — Take the governor-configured action.  
 *  
 * Called from the pretimeout tick after NMI delivery.  
 * The action depends on the current governor setting:  
 *   PANIC → call panic() with a descriptive message  
 *   DUMP  → print full register dump, do NOT reset  
 *   NONE  → just print a warning  
 */  
void wdt_pretimeout_action(void)  
{  
    switch (g_pretimeout_governor) {  
    case WDT_GOVERNOR_PANIC:  
        kprintf("[watchdog] PRETIMEOUT — governor=panic, calling panic()\n");  
        panic("Watchdog pretimeout — system will reset");  
        /* Never reached */  
        break;  

    case WDT_GOVERNOR_DUMP:  
        kprintf("[watchdog] PRETIMEOUT — governor=dump, dumping CPU state\n");  
        wdt_dump_all_cpu_regs();  
        kprintf("[watchdog] Dump complete — system continues (no reset)\n");  
        break;  

    case WDT_GOVERNOR_NONE:  
    default:  
        kprintf("[watchdog] PRETIMEOUT — governor=none, no action taken\n");  
        break;  
    }  
}  

/*  
 * wdt_dump_all_cpu_regs — Dump register state of all CPUs.  
 *  
 * In SMP mode, we send NMI backtrace requests to other CPUs via the  
 * nmi_watchdog infrastructure, then dump the local CPU's registers.  
 * In UP mode, just dump the local registers.  
 *  
 * This does NOT reset the system — it is used by the "dump" governor.  
 */  
void wdt_dump_all_cpu_regs(void)  
{  
    kprintf("=== WATCHDOG PRETIMEOUT CPU DUMP ===\n");  

    /* In SMP, trigger backtrace on all other CPUs to capture their state */  
    if (smp_get_cpu_count() > 1) {  
        kprintf("Requesting backtrace from other CPUs...\n");  
        nmi_watchdog_request_backtrace();  
    }  

    /* Dump local CPU register state */  
    dump_regs();  
    dump_stack();  

    /* Also save to pstore for post-mortem analysis */  
    {  
        extern int pstore_write(uint8_t type, const uint8_t *data, int len);  
        const char *msg = "WATCHDOG PRETIMEOUT DUMP";  
        pstore_write(PSTORE_TYPE_EMERG, (const uint8_t *)msg,  
                     (int)strlen(msg));  
    }  

    kprintf("=== END OF CPU DUMP ===\n");  
}

void watchdog_init(int timeout_seconds) {
    if (g_watchdog_active) {
        watchdog_stop();
    }

    if (timeout_seconds <= 0) timeout_seconds = 10;

    g_watchdog_timeout_ticks = (uint64_t)timeout_seconds * TIMER_FREQ;
    g_watchdog_active = 1;
    g_pretimeout_fired = 0;

    /* Schedule the initial watchdog timer */
    g_watchdog_timer_id = timer_schedule(watchdog_tick, NULL, g_watchdog_timeout_ticks);

    /* Schedule pretimeout if configured */
    if (g_pretimeout_secs > 0 && g_pretimeout_secs < timeout_seconds) {
        int pretimeout_ticks = (uint64_t)(timeout_seconds - g_pretimeout_secs) * TIMER_FREQ;
        g_pretimeout_timer_id = timer_schedule(watchdog_pretimeout_tick, NULL, pretimeout_ticks);
    }

    kprintf("[OK] Watchdog initialized (%d seconds timeout)\n", timeout_seconds);
}

void watchdog_pet(void) {
    if (!g_watchdog_active) return;

    /* Cancel the old timers and schedule new ones */
    if (g_watchdog_timer_id >= 0) {
        timer_cancel(g_watchdog_timer_id);
    }
    if (g_pretimeout_timer_id >= 0) {
        timer_cancel(g_pretimeout_timer_id);
        g_pretimeout_timer_id = -1;
    }

    g_watchdog_timer_id = timer_schedule(watchdog_tick, NULL, g_watchdog_timeout_ticks);

    /* Reset pretimeout fired flag */
    g_pretimeout_fired = 0;

    /* Re-schedule pretimeout if configured */
    if (g_pretimeout_secs > 0 && g_pretimeout_fn) {
        int timeout_secs = g_watchdog_timeout_ticks / TIMER_FREQ;
        if (g_pretimeout_secs < timeout_secs) {
            int pretimeout_ticks = (uint64_t)(timeout_secs - g_pretimeout_secs) * TIMER_FREQ;
            g_pretimeout_timer_id = timer_schedule(watchdog_pretimeout_tick, NULL, pretimeout_ticks);
        }
    }
}

void watchdog_stop(void) {
    if (!g_watchdog_active) return;

    g_watchdog_active = 0;
    if (g_watchdog_timer_id >= 0) {
        timer_cancel(g_watchdog_timer_id);
        g_watchdog_timer_id = -1;
    }
    if (g_pretimeout_timer_id >= 0) {
        timer_cancel(g_pretimeout_timer_id);
        g_pretimeout_timer_id = -1;
    }

    kprintf("[OK] Watchdog stopped\n");
}

void watchdog_set_pretimeout(int secs) {
    g_pretimeout_secs = secs;
}

void watchdog_set_pretimeout_fn(watchdog_pretimeout_fn_t fn) {
    g_pretimeout_fn = fn;
}

/*  
 * ── Sysfs interface ──────────────────────────────────────────────────  
 *  
 * Exposes under /sys/class/watchdog/watchdog0/:  
 *   pretimeout           — pretimeout value in seconds (read/write)  
 *   pretimeout_governor  — current governor: panic, dump, none (read/write)  
 *   timeout              — watchdog timeout in seconds (read-only)  
 */  

/* Sysfs read callback for /sys/class/watchdog/watchdog0/pretimeout */  
static int sysfs_pretimeout_read(char *buf, uint32_t max_size, void *priv)  
{  
    (void)priv;  
    return snprintf(buf, max_size, "%d\n", g_pretimeout_secs);  
}  

/* Sysfs write callback for /sys/class/watchdog/watchdog0/pretimeout */  
static int sysfs_pretimeout_write(const char *data, uint32_t size, void *priv)  
{  
    (void)priv;  
    if (size == 0) return -1;  
    int val = 0;  
    for (uint32_t i = 0; i < size && data[i] >= '0' && data[i] <= '9'; i++)  
        val = val * 10 + (data[i] - '0');  
    if (val < 0) val = 0;  
    g_pretimeout_secs = val;  
    return 0;  
}  

/* Sysfs read callback for /sys/class/watchdog/watchdog0/pretimeout_governor */  
static int sysfs_governor_read(char *buf, uint32_t max_size, void *priv)  
{  
    (void)priv;  
    const char *name;  
    switch (g_pretimeout_governor) {  
    case WDT_GOVERNOR_PANIC: name = "panic"; break;  
    case WDT_GOVERNOR_DUMP:  name = "dump";  break;  
    case WDT_GOVERNOR_NONE:  
    default:                 name = "none";  break;  
    }  
    return snprintf(buf, max_size, "%s\n", name);  
}  

/* Sysfs write callback for /sys/class/watchdog/watchdog0/pretimeout_governor */  
static int sysfs_governor_write(const char *data, uint32_t size, void *priv)  
{  
    (void)priv;  
    if (size == 0) return -1;  

    if (size >= 5 && (data[0] == 'p' || data[0] == 'P')) {  
        g_pretimeout_governor = WDT_GOVERNOR_PANIC;  
    } else if (size >= 4 && (data[0] == 'd' || data[0] == 'D')) {  
        g_pretimeout_governor = WDT_GOVERNOR_DUMP;  
    } else {  
        g_pretimeout_governor = WDT_GOVERNOR_NONE;  
    }  
    return 0;  
}  

/* Sysfs read callback for /sys/class/watchdog/watchdog0/timeout */  
static int sysfs_timeout_read(char *buf, uint32_t max_size, void *priv)  
{  
    (void)priv;  
    int secs = g_watchdog_timeout_ticks / TIMER_FREQ;  
    return snprintf(buf, max_size, "%d\n", secs);  
}  

void watchdog_sysfs_init(void)  
{  
    /* Create /sys/class/watchdog/ directory */  
    if (sysfs_create_dir("/sys/class/watchdog") < 0) {  
        /* Already exists — that's fine */  
    }  

    /* Create /sys/class/watchdog/watchdog0/ directory */  
    if (sysfs_create_dir("/sys/class/watchdog/watchdog0") < 0) {  
        kprintf("[watchdog] sysfs: watchdog0 dir already exists\n");  
    }  

    /* Create pretimeout attribute (writable) */  
    if (sysfs_create_writable_file(  
            "/sys/class/watchdog/watchdog0/pretimeout",  
            "0\n", NULL,  
            sysfs_pretimeout_read, sysfs_pretimeout_write) < 0) {  
        kprintf("[watchdog] sysfs: failed to create pretimeout attr\n");  
    }  

    /* Create pretimeout_governor attribute (writable) */  
    if (sysfs_create_writable_file(  
            "/sys/class/watchdog/watchdog0/pretimeout_governor",  
            "none\n", NULL,  
            sysfs_governor_read, sysfs_governor_write) < 0) {  
        kprintf("[watchdog] sysfs: failed to create pretimeout_governor attr\n");  
    }  

    /* Create timeout attribute (read-only) */  
    char timeout_str[16];  
    int secs = g_watchdog_timeout_ticks / TIMER_FREQ;  
    int n = snprintf(timeout_str, sizeof(timeout_str), "%d\n", secs);  
    if (n > 0) {  
        sysfs_create_file("/sys/class/watchdog/watchdog0/timeout", timeout_str);  
    } else {  
        sysfs_create_file("/sys/class/watchdog/watchdog0/timeout", "0\n");  
    }  

    kprintf("[OK] Watchdog sysfs interface initialized\n");  
}  

/*  
 * watchdog_system_reset — Multi-method machine reset usable from panic
 *
 * Attempts the following reset methods in order:
 *   1. ACPI reset register (via acpi_reboot)
 *   2. Keyboard controller (0x64, 0xFE)
 *   3. Legacy chipset reset ports (0x604 BX_RST, 0xB004)
 *   4. Triple-fault by loading an IDT with zero limit
 *   5. Infinite halt as last resort
 *
 * This function never returns.
 */
__attribute__((noreturn))
void watchdog_system_reset(void)
{
    /* Attempt ACPI reset register (FADT RESET_REG) if available */
    extern int acpi_find_reset_register(void);
    if (acpi_find_reset_register()) {
        extern void acpi_reboot(void);
        acpi_reboot();
        /* If we're still alive, acpi_reboot failed — continue */
    }

    kprintf("watchdog: Trying keyboard controller reset...\n");

    /* Method 2: Keyboard controller (standard PC/AT reset) */
    cli();
    for (int i = 0; i < 3; i++) {
        outb(0x64, 0xFE);
        io_wait();
    }

    kprintf("watchdog: Trying legacy chipset reset ports...\n");

    /* Method 3: Legacy chipset reset ports (Bochs/QEMU, real hw) */
    outw(0x604, 0x2000); /* BX_RST — Bochs/QEMU reset port */
    io_wait();
    outw(0xB004, 0x2000); /* QEMU/KVM alternative */
    io_wait();
    outw(0xCF9, 0x06);    /* Intel ICH/PCH reset control — hard reset */
    io_wait();

    kprintf("watchdog: Trying triple-fault reset...\n");

    /* Method 4: Triple fault — load zero-length IDT and trigger an interrupt.
     * The CPU will attempt to read the IDT, get a limit=0 fault, which in
     * protected mode triggers a triple-fault and resets the machine. */
    __asm__ volatile(
        "movw $0, %%ax\n\t"
        "lidt (%%rax)\n\t"
        "int3\n\t"
        : : : "memory"
    );

    kprintf("watchdog: All reset methods failed — system halted\n");

    /* Last resort */
    for (;;) hlt();
}
#include "module.h"
module_init(watchdog_sysfs_init);

/* I6300ESB register base (hardware-specific, may need platform detection) */
#define I6300ESB_BASE 0x0

int watchdog_start(void)
{
    kprintf("[watchdog] Starting\n");
    outb(I6300ESB_BASE + 0x05, 0x30);
    outb(I6300ESB_BASE + 0x05, 0x30);
    return 0;
}

int watchdog_set_timeout(uint32_t seconds)
{
    if (seconds == 0 || seconds > 255) return -EINVAL;
    outb(I6300ESB_BASE + 0x07, (uint8_t)seconds);
    return 0;
}

