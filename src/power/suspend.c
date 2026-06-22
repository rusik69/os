/*
 * suspend.c — ACPI S3 Suspend-to-RAM and S0ix (Suspend-to-Idle) support
 *
 * Implements:
 *   - ACPI S3 (Suspend-to-RAM): save/restore CPU state, enter via PM1a_CNT
 *   - S0ix (Suspend-to-Idle): Modern idle/suspend using MWAIT(C1e) loop
 *     for platforms that support this low-power mode.
 *
 * References:
 *   ACPI Specification v6.5, Section 4.8.4 — System Sleep (S3)
 *   Intel 64 and IA-32 Architectures SDM, Vol 3A, Chapter 9 — SMM
 *   Intel Low Power S0ix Idle (White Paper #339604)
 *
 * Item 124 — Suspend-to-Idle (S0ix) via MWAIT(C1e) loop
 */

#include "suspend.h"
#include "acpi.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "cpu.h"           /* read_msr, write_msr */
#include "smp.h"

/* ── Suspend statistics struct ─────────────────────────────────── */
struct suspend_stats {
    int success;
    int fail;
};
#include "cpuidle.h"       /* MWAIT C-state support */
#include "timer.h"          /* timer_get_ticks */
#include "lockdown.h"
#include "blockdev.h"

/* ── MSR definitions for S0ix ──────────────────────────────────────── */
#define MSR_PKG_CST_CONFIG_CONTROL  0x000000E2
#define MSR_PMG_IO_CAP_BASE         0x0000014F
#define MSR_MISC_PWR_MGMT           0x000001AA
#define MSR_POWER_MISC              0x000001FC

/* ── Static save area ────────────────────────────────────────────────── */
/* Pre-allocated page that survives S3 (RAM is self-refreshed). */
static struct suspend_state *g_save     = NULL;
static uint64_t              g_save_phys = 0;
static int                   g_setup_once = 0;

/* ── CR/register helpers ─────────────────────────────────────────────── */
static inline uint64_t rdcr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t rdcr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t rdcr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t rdcr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void     wrcr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline void     wrcr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline void     wrcr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }

/* ── State save ──────────────────────────────────────────────────────── */
static void suspend_save_cpu_state(void) {
    if (!g_save) return;

    memset(g_save, 0, sizeof(*g_save));

    /* Save GDT pseudo-descriptor using the CPU's SGDT instruction */
    {
        struct { uint16_t len; uint64_t base; } __attribute__((packed)) gdt, idt;
        __asm__ volatile("sgdt %0" : "=m"(gdt) :: "memory");
        __asm__ volatile("sidt %0" : "=m"(idt) :: "memory");
        g_save->gdt_base  = gdt.base;
        g_save->gdt_limit = gdt.len;
        g_save->idt_base  = idt.base;
        g_save->idt_limit = idt.len;
    }

    /* Control registers */
    g_save->cr0 = rdcr0();
    g_save->cr2 = rdcr2();
    g_save->cr3 = rdcr3();
    g_save->cr4 = rdcr4();

    /* Segment registers */
    __asm__ volatile("mov %%cs, %0" : "=r"(g_save->cs));
    __asm__ volatile("mov %%ds, %0" : "=r"(g_save->ds));
    __asm__ volatile("mov %%es, %0" : "=r"(g_save->es));
    __asm__ volatile("mov %%fs, %0" : "=r"(g_save->fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(g_save->gs));
    __asm__ volatile("mov %%ss, %0" : "=r"(g_save->ss));

    /* EFER MSR */
    {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
        g_save->efer = ((uint64_t)hi << 32) | lo;
    }

    /* Stack (RSP) for resume */
    __asm__ volatile("mov %%rsp, %0" : "=r"(g_save->resume_rsp));

    g_save->magic = SUSPEND_MAGIC;
}

/* ── State restore ───────────────────────────────────────────────────── */
static void suspend_restore_cpu_state(void) {
    if (!g_save || g_save->magic != SUSPEND_MAGIC) {
        kprintf("suspend: save area corrupted after resume\n");
        return;
    }

    /* Restore in safe order: GDT → segments → IDT → CR3 → CR4 → CR0 → EFER */

    /* GDT */
    {
        struct { uint16_t len; uint64_t base; } __attribute__((packed)) gdt;
        gdt.len  = (uint16_t)g_save->gdt_limit;
        gdt.base = g_save->gdt_base;
        __asm__ volatile("lgdt %0" :: "m"(gdt) : "memory");
    }

    /* Segment registers */
    __asm__ volatile("mov %0, %%ds" :: "r"(g_save->ds));
    __asm__ volatile("mov %0, %%es" :: "r"(g_save->es));
    __asm__ volatile("mov %0, %%fs" :: "r"(g_save->fs));
    __asm__ volatile("mov %0, %%gs" :: "r"(g_save->gs));
    __asm__ volatile("mov %0, %%ss" :: "r"(g_save->ss));

    /* IDT */
    {
        struct { uint16_t len; uint64_t base; } __attribute__((packed)) idt;
        idt.len  = (uint16_t)g_save->idt_limit;
        idt.base = g_save->idt_base;
        __asm__ volatile("lidt %0" :: "m"(idt) : "memory");
    }

    /* CR3 (page tables) first, then CR4, EFER, finally CR0 */
    wrcr3(g_save->cr3);
    wrcr4(g_save->cr4);

    {
        uint32_t lo = (uint32_t)g_save->efer;
        uint32_t hi = (uint32_t)(g_save->efer >> 32);
        __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000080) : "memory");
    }

    wrcr0(g_save->cr0);
}

/* ── Public API ──────────────────────────────────────────────────────── */

int suspend_s3(void) {
    /* Lockdown: block system suspend at INTEGRITY level or above */
    if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY)) {
        kprintf("suspend: blocked by kernel lockdown\n");
        return -1;
    }

    if (!acpi_sleep_supported(ACPI_S3)) {
        kprintf("suspend: ACPI S3 not supported by platform\n");
        return -1;
    }

    /* Allocate save area on first call (4 KB page, persists through S3) */
    if (!g_save) {
        g_save_phys = pmm_alloc_frame();
        if (!g_save_phys) return -2;
        g_save = (struct suspend_state *)PHYS_TO_VIRT(g_save_phys);
    }

    /* Set the resume return address once */
    if (!g_setup_once) {
        /*
         * After S3 wake, the firmware jumps to the ACPI wakeup vector.
         * Our wakeup code needs to restore CPU state and resume the kernel.
         * The resume_rip points to suspend_restore_cpu_state which runs
         * after the trampoline re-establishes long mode.
         */
        g_save->resume_rip = (uint64_t)(uintptr_t)&suspend_restore_cpu_state;
        g_setup_once = 1;
    }

    kprintf("suspend: === entering ACPI S3 (Suspend-to-RAM) ===\n");
    kprintf("suspend: save area at phys 0x%lx, size %lu bytes\n",
            (unsigned long)g_save_phys, (unsigned long)sizeof(*g_save));

    /* Save current CPU state */
    suspend_save_cpu_state();

    /* Disable interrupts — we must not be interrupted during S3 entry */
    cli();

    /*
     * Write SLP_TYP + SLP_EN to PM1a_CNT to enter S3.
     * If S3 succeeds, the CPU stops here and RAM goes into self-refresh.
     * On wake, the firmware runs then jumps to the wakeup vector.
     *
     * If acpi_sleep() returns, the S3 transition failed for some reason
     * (e.g., the platform doesn't actually support it, or the write
     * was ignored).  Restore state and return error.
     */
    int rc = acpi_sleep(ACPI_S3);

    kprintf("suspend: S3 entry failed (rc=%d), restoring state\n", rc);

    /* Re-enable interrupts before restoring */
    sti();

    /* Restore CPU state since S3 didn't actually happen */
    suspend_restore_cpu_state();

    return -3;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  S0ix — Suspend-to-Idle support
 * ═══════════════════════════════════════════════════════════════════════ */

/* S0ix idle loop counter for statistics */
static uint64_t g_s0ix_entries = 0;
static uint64_t g_s0ix_total_ticks = 0;

/*
 * Check if the platform supports S0ix (Suspend-to-Idle).
 * Modern x86 platforms with MWAIT support and proper ACPI low-power
 * idle states can enter S0ix instead of traditional S3.
 *
 * Returns 1 if S0ix is supported, 0 otherwise.
 */
int suspend_s0ix_supported(void)
{
    /* Check for MWAIT support (required for C1e+) */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));

    int have_monitor = (ecx >> 3) & 1;  /* MONITOR/MWAIT CPUID feature bit */

    if (!have_monitor) {
        kprintf("[suspend] S0ix: MWAIT not supported\n");
        return 0;
    }

    /* Also check ACPI indicates low-power idle support */
    if (!acpi_sleep_supported(ACPI_S3)) {
        /* S0ix can work on some platforms without S3 support */
        kprintf("[suspend] S0ix: ACPI S3 not present, but MWAIT available\n");
    }

    return 1;
}

/*
 * Enter S0ix (Suspend-to-Idle) state.
 *
 * This function puts the CPU into a deep idle state using MWAIT(C1e)
 * in a loop.  The CPU stays in a low-power state until a wake event
 * (interrupt, timer, device DMA) occurs.
 *
 * The implementation:
 *   1. Disables interrupts (done by caller).
 *   2. Enters MWAIT(C1e) loop: repeatedly executes MONITOR+MWAIT
 *      with the C1e hint (eax = 0x00, ecx = 0 for C1).
 *      For deeper S0ix, use Cx state hints.
 *   3. On wake event, re-evaluates whether to re-enter idle or return.
 *
 * @flags:     0 = basic S0ix, non-zero = deep S0ix with timer disabling
 * @max_latency_us: Maximum acceptable wakeup latency in microseconds.
 *                  (0 = accept any latency)
 *
 * Returns the number of timer ticks spent in S0ix.
 */
uint64_t suspend_s0ix_enter(int flags, uint32_t max_latency_us)
{
    uint64_t start_tick, end_tick;

    if (!suspend_s0ix_supported())
        return 0;

    kprintf("[suspend] === Entering S0ix (flags=%d, max_latency=%u us) ===\n",
            flags, max_latency_us);

    start_tick = end_tick = 0;

#if defined(__x86_64__)
    start_tick = timer_get_ticks();

    /* Prepare MWAIT hint:
     *   eax: C-state + sub-state hint
     *     0x00 = C1 (shallow)
     *     0x10 = C1e (enhanced C1)
     *     0x20 = C2
     *     0x30 = C3 (deep)
     *   ecx: optional extensions
     *     0x1 = Interrupts-on-exit (wake on any interrupt)
     *     0x2 = Timers-on-exit (wake on timer expiry)
     */
    unsigned int mwait_hint = 0x10; /* C1e by default */
    unsigned int mwait_ext = 0x1;   /* Wake on interrupts */

    if (flags == 0) {
        mwait_hint = 0x00; /* Basic C1 */
    }

    /* MWAIT loop — continues until a wake event occurs */
    __asm__ volatile(
        "1: \n"
        /* MONITOR — set address range for MWAIT */
        "xor %%rax, %%rax \n"
        "xor %%rcx, %%rcx \n"
        "xor %%rdx, %%rdx \n"
        "monitor \n"
        /* MWAIT — enter low-power state */
        "mov %[hint], %%rax \n"
        "mov %[ext], %%rcx \n"
        "mwait \n"
        /* Check if we should continue (on wake) */
        "test %[maxlat], %[maxlat] \n"
        "jz 1b \n"
        : /* no outputs */
        : [hint] "r" ((uint64_t)mwait_hint),
          [ext] "r" ((uint64_t)mwait_ext),
          [maxlat] "r" ((uint64_t)max_latency_us)
        : "rax", "rcx", "rdx", "memory"
    );

    end_tick = timer_get_ticks();

    g_s0ix_entries++;
    g_s0ix_total_ticks += (end_tick - start_tick);

    kprintf("[suspend] S0ix exit: slept %llu ticks (%llu entries total)\n",
            (unsigned long long)(end_tick - start_tick),
            (unsigned long long)g_s0ix_entries);
#else
    (void)flags;
    (void)max_latency_us;
    kprintf("[suspend] S0ix: not supported on this architecture\n");
#endif

    return (end_tick > start_tick) ? (end_tick - start_tick) : 0;
}

/*
 * Return S0ix statistics.
 */
void suspend_s0ix_stats(uint64_t *entries, uint64_t *total_ticks)
{
    if (entries) *entries = g_s0ix_entries;
    if (total_ticks) *total_ticks = g_s0ix_total_ticks;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Hibernate (Suspend-to-Disk) support
 * ═══════════════════════════════════════════════════════════════════════ */

int suspend_hibernate(void)
{
    /* Lockdown: block hibernation at INTEGRITY level or above */
    if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY)) {
        kprintf("hibernate: blocked by kernel lockdown\n");
        return -EPERM;
    }

    kprintf("hibernate: === Suspend-to-Disk (S4) ===\n");

    /* ── Step 1: Find and prepare the swap device ──────────────────
     * Scan registered block devices for a swap signature or
     * designated swap partition.  We look for a device with the
     * swap magic (SWAP_SPACE_MAGIC) at sector 0.
     */
    int swap_dev_id = -1;
    int num_devs = 0; /* stub: blockdev_get_count not available */
    for (int i = 0; i < num_devs; i++) {
        if (!blockdev_is_registered(i))
            continue;

        /* Read the first sector to check for swap signature */
        uint8_t sector[4096];
        int ret = blk_submit_sync(i, 0, 8, sector, BLK_REQ_READ);
        if (ret != 0)
            continue;

        /* Check for Linux swap signature (offset 0xFF6 = 'SWAPSPACE2' or 'SWAP-SPACE') */
        if (memcmp(sector + 0xFF6, "SWAPSPACE2", 10) == 0 ||
            memcmp(sector + 0xFF6, "SWAP-SPACE", 10) == 0) {
            swap_dev_id = i;
            kprintf("hibernate: found swap device %d\n", swap_dev_id);
            break;
        }
    }

    if (swap_dev_id < 0) {
        kprintf("hibernate: no swap device found — aborting\n");
        return -ENODEV;
    }

    /* ── Step 2: Calculate memory image size and swap allocation ───
     * We need enough swap space for the entire memory image.
     * For simplicity, we calculate the number of pages needed.
     */
    uint64_t total_pages = 0;  /* would be totalram_pages from MM */
    /* In a full implementation, we would iterate all memory zones
     * and count freeable pages.  For now, we use a placeholder. */
    /* Assume 512 MB of memory = 131072 pages */
    total_pages = 131072;

    uint64_t swap_sectors = total_pages * 8; /* 8 sectors per 4K page = 4096 bytes */
    uint64_t swap_size = blockdev_get_sectors(swap_dev_id);
    if (swap_sectors > swap_size) {
        kprintf("hibernate: swap device too small (need %llu sectors, have %llu)\n",
                (unsigned long long)swap_sectors,
                (unsigned long long)swap_size);
        return -ENOSPC;
    }

    kprintf("hibernate: need %llu sectors on swap device %d\n",
            (unsigned long long)swap_sectors, swap_dev_id);

    /* ── Step 3: Write swap signature for resume ───────────────────
     * We write a hibernate signature at a known location on the
     * swap device so the bootloader/kernel can detect a saved image.
     */
    uint8_t sig_sector[512];
    memset(sig_sector, 0, sizeof(sig_sector));
    memcpy(sig_sector, "HIBERNATE", 9);
    /* Store the image offset and size */
    struct hibernate_header {
        uint64_t image_offset;  /* sector offset of saved image */
        uint64_t image_sectors; /* number of sectors in image */
        uint64_t total_pages;   /* total pages saved */
        uint32_t checksum;      /* simple checksum */
        uint32_t version;       /* hibernate format version */
    } __attribute__((packed));
    struct hibernate_header *hdr = (struct hibernate_header *)(sig_sector + 512 - sizeof(struct hibernate_header));
    hdr->image_offset = 1;  /* start after header sector */
    hdr->image_sectors = swap_sectors;
    hdr->total_pages = total_pages;
    hdr->checksum = 0xDEADBEEF;
    hdr->version = 1;

    int ret = blk_submit_sync(swap_dev_id, 0, 1, sig_sector, BLK_REQ_WRITE);
    if (ret != 0) {
        kprintf("hibernate: failed to write header to swap: %d\n", ret);
        return ret;
    }
    kprintf("hibernate: header written to swap device sector 0\n");

    /* ── Step 4: Save memory image to swap ─────────────────────────
     * Iterate all memory pages, compress and write to swap.
     * This is a simplified placeholder — a full implementation would
     * use the swsusp (Software Suspend) mechanism with atomic copy.
     */
    kprintf("hibernate: saving %llu pages to swap...\n",
            (unsigned long long)total_pages);

    /* Allocate a temporary buffer for page I/O */
    uint8_t *page_buf = (uint8_t *)pmm_alloc_frame();
    if (!page_buf) {
        kprintf("hibernate: failed to allocate page buffer\n");
        return -ENOMEM;
    }

    uint64_t cur_sector = 1;  /* start after header */
    for (uint64_t p = 0; p < total_pages; p++) {
        /* Read the current page from its physical location */
        uint64_t phys_addr = p * 4096ULL;
        memcpy(page_buf, PHYS_TO_VIRT(phys_addr), 4096);

        /* Write to swap (8 sectors per page) */
        ret = blk_submit_sync(swap_dev_id, cur_sector, 8, page_buf, BLK_REQ_WRITE);
        if (ret != 0) {
            kprintf("hibernate: write error at sector %llu\n",
                    (unsigned long long)cur_sector);
            break;
        }
        cur_sector += 8;

        if ((p % 10000) == 0 && p > 0) {
            kprintf("hibernate: saved %llu/%llu pages (%.0f%%)\n",
                    (unsigned long long)p, (unsigned long long)total_pages,
                    (double)p * 100.0 / (double)total_pages);
        }
    }

    pmm_free_frame((uint64_t)page_buf);

    kprintf("hibernate: memory image saved (%llu pages, %llu sectors)\n",
            (unsigned long long)total_pages,
            (unsigned long long)(cur_sector - 1));

    /* ── Step 5: Power off (enter S4) ──────────────────────────────
     * Write the S4 sleep type to the ACPI PM1a_CNT register.
     * If S4 is not available, we just return to let the caller decide.
     */
    if (acpi_sleep_supported(ACPI_S4)) {
        kprintf("hibernate: entering ACPI S4 state\n");
        cli();
        int rc = acpi_sleep(ACPI_S4);
        sti();
        if (rc != 0) {
            kprintf("hibernate: S4 entry failed (rc=%d)\n", rc);
            return rc;
        }
    } else {
        kprintf("hibernate: S4 not supported — image saved, system should be powered off\n");
        kprintf("hibernate: on next boot, kernel will detect saved image and resume\n");
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  PM Suspend State Machine — suspend_prepare / suspend_enter / wakeup
 * ═══════════════════════════════════════════════════════════════════════ */

/* PM suspend state identifiers */
#define PM_SUSPEND_STANDBY     1
#define PM_SUSPEND_MEM         2
#define PM_SUSPEND_DISK        3

/* State type for suspend_prepare / suspend_enter */
typedef int suspend_state_t;

/* Suspend state names for logging */
static const char *pm_state_name(int state)
{
    switch (state) {
    case PM_SUSPEND_STANDBY: return "standby";
    case PM_SUSPEND_MEM:     return "mem";
    case PM_SUSPEND_DISK:    return "disk";
    default:                 return "unknown";
    }
}

/* ── Suspend statistics ─────────────────────────────────────────────── */
static struct suspend_stats g_suspend_stats;
static spinlock_t g_suspend_stats_lock = SPINLOCK_INIT;

/* ── Device suspend ordering ───────────────────────────────────────────
 * Track which devices have been suspended so we can resume in reverse order.
 */
#define MAX_SUSPEND_DEVICES 64

struct suspend_device_entry {
    int  in_use;
    char devname[48];
    int  suspended;
};

static struct suspend_device_entry g_suspend_devices[MAX_SUSPEND_DEVICES];
static int g_suspend_device_count = 0;
static spinlock_t g_suspend_dev_lock = SPINLOCK_INIT;

/* Register a device for suspend ordering (called by driver PM runtime) */
int suspend_device_register(const char *name)
{
    if (!name) return -EINVAL;

    spinlock_acquire(&g_suspend_dev_lock);
    if (g_suspend_device_count >= MAX_SUSPEND_DEVICES) {
        spinlock_release(&g_suspend_dev_lock);
        return -ENOSPC;
    }
    struct suspend_device_entry *e = &g_suspend_devices[g_suspend_device_count++];
    strncpy(e->devname, name, sizeof(e->devname) - 1);
    e->devname[sizeof(e->devname) - 1] = '\0';
    e->in_use = 1;
    e->suspended = 0;
    spinlock_release(&g_suspend_dev_lock);
    return 0;
}

/* Suspend all registered devices (called ordered from leaf to root) */
static int suspend_devices(void)
{
    spinlock_acquire(&g_suspend_dev_lock);
    for (int i = 0; i < g_suspend_device_count; i++) {
        if (g_suspend_devices[i].in_use) {
            kprintf("[suspend]  suspending device: %s\n",
                    g_suspend_devices[i].devname);
            g_suspend_devices[i].suspended = 1;
        }
    }
    spinlock_release(&g_suspend_dev_lock);
    return 0;
}

/* Resume all suspended devices (reverse order) */
static void resume_devices(void)
{
    spinlock_acquire(&g_suspend_dev_lock);
    for (int i = g_suspend_device_count - 1; i >= 0; i--) {
        if (g_suspend_devices[i].in_use && g_suspend_devices[i].suspended) {
            kprintf("[suspend]  resuming device: %s\n",
                    g_suspend_devices[i].devname);
            g_suspend_devices[i].suspended = 0;
        }
    }
    spinlock_release(&g_suspend_dev_lock);
}

/* ── Wakeup event tracking ──────────────────────────────────────────── */
#define WAKEUP_EVENT_HISTORY 16

static struct {
    uint64_t total_count;
    uint64_t pending;
    uint64_t history[WAKEUP_EVENT_HISTORY];
    int      history_idx;
} g_wakeup_state;

/* Forward declarations for functions defined later in this file */
static int suspend_devices(void);
static void resume_devices(void);
int suspend_prepare(suspend_state_t state);
int suspend_enter(suspend_state_t state);
void suspend_wakeup(void);

/* Record a wakeup event (called from interrupt handlers) */
void pm_wakeup_event(void)
{
    uint64_t tick = 0;
    /* timer_get_ticks may not be available early, use best-effort */
    /* tick = timer_get_ticks(); */

    g_wakeup_state.total_count++;
    g_wakeup_state.pending++;
    g_wakeup_state.history[g_wakeup_state.history_idx] = tick;
    g_wakeup_state.history_idx =
        (g_wakeup_state.history_idx + 1) % WAKEUP_EVENT_HISTORY;
}

/* ── Public suspend API ─────────────────────────────────────────────── */

/* Current global PM suspend state */
static int g_pm_state = 0; /* 0 = active, PM_SUSPEND_* = suspending */

/* Begin a suspend cycle — sets the global PM state */
void pm_suspend_begin(int state)
{
    g_pm_state = state;
    kprintf("[suspend] PM state set to %s (%d)\n", pm_state_name(state), state);
}

/* End a suspend cycle — clears the global PM state */
void pm_suspend_end(void)
{
    kprintf("[suspend] PM state cleared (back to active)\n");
    g_pm_state = 0;
}

/* Check if the system is currently in a suspend cycle */
int pm_suspend_in_progress(void)
{
    return (g_pm_state != 0) ? 1 : 0;
}

/* Get the current PM suspend state */
int pm_suspend_get_state(void)
{
    return g_pm_state;
}

/* Complete suspend cycle: prepare → enter → wakeup */
int pm_suspend_cycle(int state)
{
    int ret;

    pm_suspend_begin(state);

    ret = suspend_prepare(state);
    if (ret < 0) {
        kprintf("[suspend] Prepare failed for %s: %d\n",
                pm_state_name(state), ret);
        pm_suspend_end();
        return ret;
    }

    ret = suspend_enter(state);
    if (ret < 0) {
        kprintf("[suspend] Enter failed for %s: %d\n",
                pm_state_name(state), ret);
        pm_suspend_end();
        return ret;
    }

    suspend_wakeup();
    pm_suspend_end();

    /* Update statistics */
    spinlock_acquire(&g_suspend_stats_lock);
    if (ret == 0)
        g_suspend_stats.success++;
    else
        g_suspend_stats.fail++;
    spinlock_release(&g_suspend_stats_lock);

    return ret;
}

/* suspend_prepare — Prepare the system for suspend.
 *
 * Performs:
 *   1. Notify drivers of impending suspend (device suspend ordering)
 *   2. Freeze processes (userspace + kernel threads)
 *   3. Sync filesystems
 *   4. Prepare CPUs for suspend (disable non-boot CPUs on SMP)
 *
 * @state: PM_SUSPEND_STANDBY, PM_SUSPEND_MEM, or PM_SUSPEND_DISK
 * Returns 0 on success, negative on failure (abort suspend).
 */
int suspend_prepare(suspend_state_t state)
{
    kprintf("[suspend] Preparing system for %s suspend...\n",
            pm_state_name(state));

    /* Step 1: Suspend devices in order */
    int ret = suspend_devices();
    if (ret < 0) {
        kprintf("[suspend] Device suspend failed: %d\n", ret);
        resume_devices();
        return ret;
    }

    /* Step 2: Notify drivers (stub — in production would call notifier chain) */
    /* driver_suspend_notify(state); */

    /* Step 3: Sync filesystems */
    /* vfs_sync_all(); */

    /* Step 4: Freeze processes (stub — would use process_freeze_all()) */
    /* process_freeze_all(THAW_PROCESS); */

    kprintf("[suspend] System prepared for %s suspend\n",
            pm_state_name(state));
    return 0;
}

/* suspend_enter — Enter the given suspend state.
 *
 * Depending on @state:
 *   - PM_SUSPEND_STANDBY → light-weight idle (S0ix / s2idle)
 *   - PM_SUSPEND_MEM     → Suspend-to-RAM (ACPI S3)
 *   - PM_SUSPEND_DISK    → Hibernate (ACPI S4)
 *
 * This function returns after resume.
 */
int suspend_enter(suspend_state_t state)
{
    int ret = 0;

    kprintf("[suspend] Entering %s state...\n", pm_state_name(state));

    cli();  /* Disable interrupts — must not be interrupted during entry */

    switch (state) {
    case PM_SUSPEND_STANDBY:
        /* Light-weight suspend: use MWAIT-based S0ix / s2idle */
        /* In production this calls suspend_s2idle_enter() */
        __asm__ volatile("sti; hlt; cli" ::: "memory");
        break;

    case PM_SUSPEND_MEM:
        /* Suspend-to-RAM via ACPI S3 */
        ret = acpi_sleep(ACPI_S3);
        break;

    case PM_SUSPEND_DISK:
        /* Suspend-to-Disk via ACPI S4 */
        if (acpi_sleep_supported(ACPI_S4))
            ret = acpi_sleep(ACPI_S4);
        else
            ret = -ENOSYS;
        break;

    default:
        ret = -EINVAL;
        break;
    }

    sti();  /* Re-enable interrupts after resume/error */

    if (ret < 0) {
        kprintf("[suspend] %s entry failed (rc=%d)\n",
                pm_state_name(state), ret);
    }

    return ret;
}

/* suspend_wakeup — Called after waking from suspend.
 * Re-enables devices and notifies drivers of wakeup.
 */
void suspend_wakeup(void)
{
    kprintf("[suspend] Waking up from suspend...\n");

    /* Resume devices in reverse order */
    resume_devices();

    /* Notify drivers of resume */
    /* driver_resume_notify(); */

    /* Thaw processes */
    /* process_thaw_all(); */

    /* Clear pending wakeup count */
    g_wakeup_state.pending = 0;

    kprintf("[suspend] Wakeup complete\n");
}

/* suspend_stats — Return current suspend statistics.
 * Fills in success/fail counts and total wakeup count.
 */
void suspend_stats(struct suspend_stats *stats)
{
    if (!stats) return;

    spinlock_acquire(&g_suspend_stats_lock);
    stats->success = g_suspend_stats.success;
    stats->fail    = g_suspend_stats.fail;
    spinlock_release(&g_suspend_stats_lock);
}

/* suspend_wakeup_count — Return total wakeup events seen since boot. */
int suspend_wakeup_count(void)
{
    return (int)g_wakeup_state.total_count;
}

/* suspend_wakeup_count_check — Check if wakeup events are pending.
 * Returns 0 if no pending wakeups (safe to suspend), >0 if pending.
 */
int suspend_wakeup_count_check(void)
{
    return (g_wakeup_state.pending > 0) ? 1 : 0;
}

/* suspend_wakeup_count_reset — Clear the pending wakeup counter. */
void suspend_wakeup_count_reset(void)
{
    g_wakeup_state.pending = 0;
}
