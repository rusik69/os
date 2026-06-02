#define KERNEL_INTERNAL
#include "types.h"
#include "panic.h"
#include "printf.h"
#include "vga.h"
#include "io.h"
#include "process.h"
#include "smp.h"
#include "timer.h"
#include "acpi.h"
#include "sysrq.h"
#include "kallsyms.h"
#include "pstore.h"
#include "kdump.h"
#include "notifier.h"
#include "watchdog.h"

/*
 * ── Panic timeout ─────────────────────────────────────────────────
 *
 * After a panic, instead of hanging forever the system will reset
 * after `panic_timeout` seconds.  Set to 0 to disable (infinite hang).
 */
int panic_timeout = 30;  /* default: reset after 30 seconds */

static uint64_t g_tsc_freq_hz = 0;  /* Calibrated TSC frequency in Hz */

/*
 * Read the x86 Time-Stamp Counter (RDTSC).
 * Serialising via CPUID to ensure monotonicity across CPUs.
 */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    /* CPUID serialisation prevents instruction reordering across RDTSC */
    __asm__ volatile(
        "cpuid\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        : "a"(0)
        : "rbx", "rcx"
    );
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/*
 * Calibrate the TSC frequency using the PIT channel 2.
 *
 * We program the PIT in one-shot mode with a known count, measure the
 * elapsed TSC ticks, and compute the frequency.  This works even with
 * interrupts disabled because the PIT is a hardware counter.
 *
 * On platforms where PIT is unavailable (e.g. HPET-only chipsets) we
 * fall back to CPUID leaf 0x15 if available, else a safe default.
 */
static void calibrate_tsc(void)
{
    /* ── Attempt 1: CPUID leaf 0x15 (TSC / core crystal clock ratio) ── */
    {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15));
        if (ecx != 0 && ebx != 0 && eax != 0) {
            /* TSC frequency = ecx * ebx / eax  (in Hz if ecx is Hz) */
            g_tsc_freq_hz = (uint64_t)ecx * (uint64_t)ebx / (uint64_t)eax;
            if (g_tsc_freq_hz > 0)
                return;
        }
    }

    /* ── Attempt 2: CPUID leaf 0x16 (Processor Base Frequency in MHz) ── */
    {
        uint32_t eax, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x16));
        if (eax > 0) {
            g_tsc_freq_hz = (uint64_t)eax * 1000000ULL;
            return;
        }
    }

    /* ── Attempt 3: PIT-based calibration ───────────────────────────── */
    {
        const uint16_t pit_count = 65535;  /* max count for longest delay */
        const uint32_t pit_rate_hz = 1193182;  /* PIT input clock */

        /* Set PIT channel 2: one-shot, mode 0, binary */
        cli();
        outb(0x43, 0xB0);
        outb(0x42, pit_count & 0xFF);
        outb(0x42, (pit_count >> 8) & 0xFF);

        /* Make sure the gate is high (bit 0 of port 0x61) */
        uint8_t gate = inb(0x61);
        outb(0x61, gate | 1);

        uint64_t tsc_start = rdtsc();

        /* Wait for the PIT to count down to zero (OUT bit = bit 5 of port 0x61) */
        while (!(inb(0x61) & 0x20));

        uint64_t tsc_end = rdtsc();

        /* Compute TSC frequency */
        uint64_t elapsed_tsc = tsc_end - tsc_start;
        /* Total time = pit_count / pit_rate_hz seconds */
        /* TSC freq = elapsed_tsc * pit_rate_hz / pit_count */
        g_tsc_freq_hz = elapsed_tsc * (uint64_t)pit_rate_hz / (uint64_t)pit_count;

        if (g_tsc_freq_hz > 0)
            return;
    }

    /* ── Fallback: assume 2 GHz ─────────────────────────────────── */
    g_tsc_freq_hz = 2000000000ULL;
}

/*
 * Return the estimated TSC frequency in Hz (cached from calibration).
 */
uint64_t panic_get_tsc_freq(void)
{
    return g_tsc_freq_hz;
}

/*
 * Set the panic timeout.  Pass 0 to disable timeout-based reset
 * (system hangs forever on panic, as before).
 */
void panic_set_timeout(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    panic_timeout = seconds;
}

/*
 * Panic halt loop — replaces the old `for (;;) hlt()`.
 *
 * If panic_timeout > 0, the system will attempt to reset after that
 * many seconds.  During the waiting period we print periodic messages
 * so the user/operator can observe the countdown.
 */
static __attribute__((noreturn))
void panic_halt_loop(void)
{
    if (panic_timeout <= 0 || g_tsc_freq_hz == 0) {
        /* Legacy behaviour: hang forever */
        for (;;) hlt();
    }

    uint64_t start_tsc = rdtsc();
    uint64_t timeout_ticks = (uint64_t)panic_timeout * g_tsc_freq_hz;
    int last_reported = 0;

    for (;;) {
        uint64_t now = rdtsc();
        uint64_t elapsed = (now - start_tsc);

        /* Check for TSC wraparound (extremely rare, but handle it) */
        if (now < start_tsc) {
            /* TSC wrapped; restart measurement */
            start_tsc = now;
            continue;
        }

        /* Report progress every 5 seconds */
        int elapsed_sec = (int)(elapsed / g_tsc_freq_hz);
        if (elapsed_sec >= last_reported + 5) {
            int remaining = panic_timeout - elapsed_sec;
            if (remaining > 0) {
                kprintf("panic: System will reset in %d seconds... (Ctrl-Alt-Del to halt)\n",
                        remaining);
            } else {
                kprintf("panic: Timeout reached — resetting system\n");
            }
            last_reported = elapsed_sec;
        }

        if (elapsed >= timeout_ticks) {
            /* Timeout expired — trigger system reset */
            watchdog_system_reset();
        }

        /* Brief pause to reduce power consumption while polling */
        for (volatile int i = 0; i < 10000; i++) {
            __asm__ volatile("pause");
        }
        io_wait();
    }
}

/*
 * Save kernel panic state to the persistent storage region (pstore)
 * for post-mortem analysis across reboots.
 *
 * Splits the dump across multiple pstore records (max 256 bytes each).
 * Callers should cli() first to ensure consistent register state.
 *
 * @rip  The actual instruction pointer where the panic was triggered
 *       (use __builtin_return_address(0) from panic()).
 */
static void kdump_save_panic(const char *msg, uint64_t actual_rip)
{
    char buf[PSTORE_MAX_DATA_LEN];
    int len;
    struct process *cur;
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rsp, rbp, rip, rflags;
    uint64_t cs, ds, es, fs, gs, ss;

    /* ── Read all CPU registers ── */
    __asm__ volatile("mov %%cr0, %0"  : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0"  : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0"  : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0"  : "=r"(cr4));
    __asm__ volatile("mov %%cr8, %0"  : "=r"(cr8));
    __asm__ volatile("mov %%rax, %0"  : "=r"(rax));
    __asm__ volatile("mov %%rbx, %0"  : "=r"(rbx));
    __asm__ volatile("mov %%rcx, %0"  : "=r"(rcx));
    __asm__ volatile("mov %%rdx, %0"  : "=r"(rdx));
    __asm__ volatile("mov %%rsi, %0"  : "=r"(rsi));
    __asm__ volatile("mov %%rdi, %0"  : "=r"(rdi));
    __asm__ volatile("mov %%r8,  %0"  : "=r"(r8));
    __asm__ volatile("mov %%r9,  %0"  : "=r"(r9));
    __asm__ volatile("mov %%r10, %0"  : "=r"(r10));
    __asm__ volatile("mov %%r11, %0"  : "=r"(r11));
    __asm__ volatile("mov %%r12, %0"  : "=r"(r12));
    __asm__ volatile("mov %%r13, %0"  : "=r"(r13));
    __asm__ volatile("mov %%r14, %0"  : "=r"(r14));
    __asm__ volatile("mov %%r15, %0"  : "=r"(r15));
    __asm__ volatile("mov %%rsp, %0"  : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0"  : "=r"(rbp));
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("mov %%cs,  %0"  : "=r"(cs));
    __asm__ volatile("mov %%ds,  %0"  : "=r"(ds));
    __asm__ volatile("mov %%es,  %0"  : "=r"(es));
    __asm__ volatile("mov %%fs,  %0"  : "=r"(fs));
    __asm__ volatile("mov %%gs,  %0"  : "=r"(gs));
    __asm__ volatile("mov %%ss,  %0"  : "=r"(ss));
    rip = actual_rip;

    /* ── Record 0: panic header (message + process info) ── */
    cur = process_get_current();
    len = snprintf(buf, sizeof(buf),
        "=== KERNEL PANIC ===\n"
        "MSG: %s\n"
        "PID=%u PROC=%s CPU=%u/%d",
        msg ? msg : "(null)",
        cur ? (unsigned int)cur->pid : 0,
        cur && cur->name ? cur->name : "?",
        smp_get_cpu_id(), smp_get_cpu_count());
    if (len > 0) {
        if (len >= (int)sizeof(buf))
            len = (int)sizeof(buf) - 1;
        pstore_write(PSTORE_TYPE_PANIC, (const uint8_t *)buf, len);
    }

    /* ── Record 1: integer/pointer registers ── */
    len = snprintf(buf, sizeof(buf),
        "RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n"
        "RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx",
        (unsigned long long)rax, (unsigned long long)rbx,
        (unsigned long long)rcx, (unsigned long long)rdx,
        (unsigned long long)rsi, (unsigned long long)rdi,
        (unsigned long long)rbp, (unsigned long long)rsp);
    if (len > 0) {
        if (len >= (int)sizeof(buf))
            len = (int)sizeof(buf) - 1;
        pstore_write(PSTORE_TYPE_PANIC, (const uint8_t *)buf, len);
    }

    /* ── Record 2: extended registers ── */
    len = snprintf(buf, sizeof(buf),
        "R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n"
        "R12=%016llx R13=%016llx R14=%016llx R15=%016llx",
        (unsigned long long)r8,  (unsigned long long)r9,
        (unsigned long long)r10, (unsigned long long)r11,
        (unsigned long long)r12, (unsigned long long)r13,
        (unsigned long long)r14, (unsigned long long)r15);
    if (len > 0) {
        if (len >= (int)sizeof(buf))
            len = (int)sizeof(buf) - 1;
        pstore_write(PSTORE_TYPE_PANIC, (const uint8_t *)buf, len);
    }

    /* ── Record 3: RIP, RFLAGS, segment + control registers ── */
    len = snprintf(buf, sizeof(buf),
        "RIP=%016llx RFL=%016llx\n"
        "CS=%04llx DS=%04llx ES=%04llx FS=%04llx GS=%04llx SS=%04llx\n"
        "CR0=%016llx CR2=%016llx CR3=%016llx CR4=%016llx CR8=%016llx",
        (unsigned long long)rip, (unsigned long long)rflags,
        cs, ds, es, fs, gs, ss,
        (unsigned long long)cr0, (unsigned long long)cr2,
        (unsigned long long)cr3, (unsigned long long)cr4,
        (unsigned long long)cr8);
    if (len > 0) {
        if (len >= (int)sizeof(buf))
            len = (int)sizeof(buf) - 1;
        pstore_write(PSTORE_TYPE_PANIC, (const uint8_t *)buf, len);
    }

    /* ── Records 4+: stack trace (one frame per record) ── */
    {
        uint64_t frame_rbp = rbp;
        int frame = 0;
        const char *sym;

        len = snprintf(buf, sizeof(buf), "Stack trace:"); /* Linux format */
        if (len > 0) {
            if (len >= (int)sizeof(buf))
                len = (int)sizeof(buf) - 1;
            pstore_write(PSTORE_TYPE_PANIC, (const uint8_t *)buf, len);
        }

        while (frame_rbp && frame_rbp >= 0xFFFF800000000000ULL && frame < 32) {
            uint64_t *frame_ptr = (uint64_t *)frame_rbp;
            uint64_t ret_addr = frame_ptr[1];
            if (ret_addr == 0)
                break;

            if (ret_addr < 0xFFFF800000000000ULL)
                sym = "(user-mode)";
            else
                sym = kallsyms_lookup(ret_addr);

            len = snprintf(buf, sizeof(buf),
                "  [%02d] %016llx %s",
                frame, (unsigned long long)ret_addr, sym);
            if (len > 0) {
                if (len >= (int)sizeof(buf))
                    len = (int)sizeof(buf) - 1;
                pstore_write(PSTORE_TYPE_PANIC, (const uint8_t *)buf, len);
            }

            frame_rbp = frame_ptr[0];
            frame++;
        }

        if (frame == 0) {
            pstore_write(PSTORE_TYPE_PANIC,
                         (const uint8_t *)"  (no frame pointers in trace)", 34);
        }
    }
}

/* ── Stack trace: walk frame pointers (RBP chain) using kallsyms ── */

void dump_stack(void) {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    kprintf("Call trace:\n");
    int frame = 0;
    while (rbp && rbp >= 0xFFFF800000000000ULL && frame < 32) {
        uint64_t ret_addr = ((uint64_t *)rbp)[1];
        if (ret_addr == 0) break;
        kprintf("  [<%016llx>] ", (unsigned long long)ret_addr);
        if (ret_addr < 0xFFFF800000000000ULL)
            kprintf("user-mode\n");
        else
            kprintf("%s\n", kallsyms_lookup(ret_addr));
        rbp = *(uint64_t *)rbp;
        frame++;
    }
    if (frame == 0)
        kprintf("  (no frame pointers in trace)\n");
}

void dump_regs(void) {
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rsp, rbp, rip, rflags;
    uint64_t cs, ds, es, fs, gs, ss;

    __asm__ volatile("mov %%cr0, %0"  : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0"  : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0"  : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0"  : "=r"(cr4));
    __asm__ volatile("mov %%cr8, %0"  : "=r"(cr8));
    __asm__ volatile("mov %%rax, %0"  : "=r"(rax));
    __asm__ volatile("mov %%rbx, %0"  : "=r"(rbx));
    __asm__ volatile("mov %%rcx, %0"  : "=r"(rcx));
    __asm__ volatile("mov %%rdx, %0"  : "=r"(rdx));
    __asm__ volatile("mov %%rsi, %0"  : "=r"(rsi));
    __asm__ volatile("mov %%rdi, %0"  : "=r"(rdi));
    __asm__ volatile("mov %%r8,  %0"  : "=r"(r8));
    __asm__ volatile("mov %%r9,  %0"  : "=r"(r9));
    __asm__ volatile("mov %%r10, %0"  : "=r"(r10));
    __asm__ volatile("mov %%r11, %0"  : "=r"(r11));
    __asm__ volatile("mov %%r12, %0"  : "=r"(r12));
    __asm__ volatile("mov %%r13, %0"  : "=r"(r13));
    __asm__ volatile("mov %%r14, %0"  : "=r"(r14));
    __asm__ volatile("mov %%r15, %0"  : "=r"(r15));
    __asm__ volatile("mov %%rsp, %0"  : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0"  : "=r"(rbp));
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("mov %%cs,  %0"  : "=r"(cs));
    __asm__ volatile("mov %%ds,  %0"  : "=r"(ds));
    __asm__ volatile("mov %%es,  %0"  : "=r"(es));
    __asm__ volatile("mov %%fs,  %0"  : "=r"(fs));
    __asm__ volatile("mov %%gs,  %0"  : "=r"(gs));
    __asm__ volatile("mov %%ss,  %0"  : "=r"(ss));

    /* RIP via return address — captures the actual caller */
    rip = (uint64_t)__builtin_return_address(0);

    kprintf("=== REGISTER DUMP ===\n");
    kprintf(" RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
            (unsigned long long)rax, (unsigned long long)rbx,
            (unsigned long long)rcx, (unsigned long long)rdx);
    kprintf(" RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n",
            (unsigned long long)rsi, (unsigned long long)rdi,
            (unsigned long long)rbp, (unsigned long long)rsp);
    kprintf(" R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
            (unsigned long long)r8,  (unsigned long long)r9,
            (unsigned long long)r10, (unsigned long long)r11);
    kprintf(" R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
            (unsigned long long)r12, (unsigned long long)r13,
            (unsigned long long)r14, (unsigned long long)r15);
    kprintf(" RIP=%016llx RFL=%016llx\n",
            (unsigned long long)rip, (unsigned long long)rflags);
    kprintf(" CS=%04llx DS=%04llx ES=%04llx FS=%04llx GS=%04llx SS=%04llx\n",
            cs, ds, es, fs, gs, ss);
    kprintf(" CR0=%016llx CR2=%016llx CR3=%016llx CR4=%016llx CR8=%016llx\n",
            (unsigned long long)cr0, (unsigned long long)cr2,
            (unsigned long long)cr3, (unsigned long long)cr4,
            (unsigned long long)cr8);
}

__attribute__((noreturn))
void panic(const char *fmt, ...) {
    char msg_buf[128];

    cli();

    /* Capture the panic message text before we start printing */
    {
        __builtin_va_list args;
        __builtin_va_start(args, fmt);
        vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
        __builtin_va_end(args);
    }

    /* Capture the real RIP at the panic call site */
    uint64_t panic_rip = (uint64_t)__builtin_return_address(0);

    /* Save state to persistent storage for post-mortem analysis */
    kdump_save_panic(msg_buf, panic_rip);

    /* Save to the dedicated kdump memory region (if available) */
    kdump_capture(msg_buf, panic_rip);

    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n\n=== KERNEL PANIC ===\n");
    kprintf("%s\n", msg_buf);

    /* Try to dump CPU state */
    struct process *cur = process_get_current();
    if (cur) {
        kprintf("Process: %s (pid=%u, state=%u)\n",
                cur->name ? cur->name : "?", cur->pid, (uint32_t)cur->state);
    }

    if (smp_get_cpu_count() > 1) {
        kprintf("CPU: %d/%d\n", smp_get_cpu_id(), smp_get_cpu_count());
    }

    dump_regs();
    dump_stack();

    /* Notify panic notifier chain (releases spinlocks, etc.) */
    notifier_call_chain(NOTIFIER_PANIC, 0, NULL);

    kprintf("=== SYSTEM HALTED (will reset in %d seconds) ===\n", panic_timeout);

    /* Enter the timeout-based halt loop (replaces old `for (;;) hlt()`) */
    panic_halt_loop();
}

void panic_init(void) {
    calibrate_tsc();

    kprintf("[OK] Panic/oops handler initialized (timeout=%ds, TSC=%llu MHz)\n",
            panic_timeout, (unsigned long long)(g_tsc_freq_hz / 1000000));
}
