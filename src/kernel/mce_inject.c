/*
 * mce_inject.c — Machine Check Exception injection for testing (Item 396)
 *
 * Provides a debugfs interface under /sys/kernel/debug/mce-inject/ that
 * allows userspace to inject simulated machine check errors for testing
 * the MCE handling path without requiring real hardware faults.
 *
 * Usage:
 *   # Set the bank number:
 *   echo 0 > /sys/kernel/debug/mce-inject/bank
 *
 *   # Set the MCi_STATUS value (must have MC_STATUS_VAL set):
 *   echo 0xBE00000000000000 > /sys/kernel/debug/mce-inject/status
 *     → 0xBE... = VAL | UC | PCC (uncorrectable fatal)
 *     → 0x9800000000000000 = VAL | UC (uncorrectable, no PCC)
 *     → 0x8000000000000000 = VAL (corrected, no action)
 *
 *   # Optionally set address and misc:
 *   echo 0x1000000 > /sys/kernel/debug/mce-inject/addr
 *   echo 0 > /sys/kernel/debug/mce-inject/misc
 *
 *   # Trigger injection (writes values to MSRs and calls #MC handler):
 *   echo 1 > /sys/kernel/debug/mce-inject/trigger
 *
 * The injector writes MSR_IA32_MCi_STATUS/ADDR/MISC for the specified
 * bank, sets MSR_IA32_MCG_STATUS.MCIP, builds a synthetic interrupt
 * frame, and calls mce_handler() directly.  After the handler returns,
 * all injected bank MSRs are cleared and MCIP is restored.
 *
 * SAFETY: This is a debug-only feature.  It bypasses normal hardware
 * error signalling and writes directly to machine check MSRs.  Never
 * enable in production or on systems with live MCE hardware capable
 * of concurrent error reporting.
 */

#include "mce.h"
#include "cpu.h"
#include "debugfs.h"
#include "printf.h"
#include "string.h"
#include "idt.h"
#include "smp.h"

/* ── Injectable state ─────────────────────────────────────────────── */

/* The bank number to inject into (0 .. num_banks-1) */
static uint32_t g_inject_bank = 0;

/* MCi_STATUS value to write (must have VAL bit set) */
static uint64_t g_inject_status = 0;

/* MCi_ADDR value (optional, only used if ADDRV set in status) */
static uint64_t g_inject_addr = 0;

/* MCi_MISC value (optional, only used if MISCV set in status) */
static uint64_t g_inject_misc = 0;

/* Running count of injections triggered (for diagnostics) */
static uint32_t g_inject_count = 0;

/* ── Helper: parse a hex or decimal 64-bit value from a string ────── */

static uint64_t parse_u64(const char *s, int len)
{
    uint64_t val = 0;
    int i = 0;

    /* Skip leading whitespace */
    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;

    if (i >= len)
        return 0;

    /* Check for hex prefix */
    int is_hex = 0;
    if (i + 2 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        is_hex = 1;
        i += 2;
    }

    for (; i < len; i++) {
        char c = s[i];
        if (c == '\n' || c == '\0')
            break;

        if (is_hex) {
            if (c >= '0' && c <= '9')
                val = (val << 4) | (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                val = (val << 4) | (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                val = (val << 4) | (uint64_t)(c - 'A' + 10);
            else
                break; /* stop on invalid character */
        } else {
            if (c >= '0' && c <= '9')
                val = val * 10 + (uint64_t)(c - '0');
            else
                break;
        }
    }

    return val;
}

/* ── Read callbacks for debugfs files ─────────────────────────────── */

static void mce_inject_bank_read(char *buf, int *len)
{
    *len = snprintf(buf, 64, "%u\n", (unsigned int)g_inject_bank);
}

static void mce_inject_status_read(char *buf, int *len)
{
    *len = snprintf(buf, 64, "0x%016llx\n", (unsigned long long)g_inject_status);
}

static void mce_inject_addr_read(char *buf, int *len)
{
    *len = snprintf(buf, 64, "0x%016llx\n", (unsigned long long)g_inject_addr);
}

static void mce_inject_misc_read(char *buf, int *len)
{
    *len = snprintf(buf, 64, "0x%016llx\n", (unsigned long long)g_inject_misc);
}

static void mce_inject_count_read(char *buf, int *len)
{
    *len = snprintf(buf, 64, "%u\n", (unsigned int)g_inject_count);
}

/* ── Write callbacks ──────────────────────────────────────────────── */

static int mce_inject_bank_write(const char *buf, int len)
{
    uint64_t val = parse_u64(buf, len);
    /* Bank number is limited to a reasonable range; actual validation
     * happens at trigger time when we read MCG_CAP. */
    if (val > 255)
        return -1;
    g_inject_bank = (uint32_t)val;
    return 0;
}

static int mce_inject_status_write(const char *buf, int len)
{
    g_inject_status = parse_u64(buf, len);
    return 0;
}

static int mce_inject_addr_write(const char *buf, int len)
{
    g_inject_addr = parse_u64(buf, len);
    return 0;
}

static int mce_inject_misc_write(const char *buf, int len)
{
    g_inject_misc = parse_u64(buf, len);
    return 0;
}

/* ── Trigger: perform the injection ───────────────────────────────── */

/*
 * Build a synthetic interrupt frame from the current CPU state.
 * We need a minimal frame that mce_handler can use to log RIP etc.
 */
static void build_synthetic_frame(struct interrupt_frame *frame)
{
    /* Zero-initialise */
    memset(frame, 0, sizeof(*frame));

    /* Capture current register state via inline assembly.
     * The __volatile__ and memory clobber prevent the compiler from
     * reordering or optimising these reads relative to the MSR writes. */
    __asm__ volatile(
        "mov %%rax, %0  \n\t"
        "mov %%rbx, %1  \n\t"
        "mov %%rcx, %2  \n\t"
        "mov %%rdx, %3  \n\t"
        "mov %%rsi, %4  \n\t"
        "mov %%rdi, %5  \n\t"
        "mov %%rbp, %6  \n\t"
        "mov %%rsp, %7  \n\t"
        "mov %%r8,  %8  \n\t"
        "mov %%r9,  %9  \n\t"
        "mov %%r10, %10 \n\t"
        "mov %%r11, %11 \n\t"
        "mov %%r12, %12 \n\t"
        "mov %%r13, %13 \n\t"
        "mov %%r14, %14 \n\t"
        "mov %%r15, %15 \n\t"
        : "=m"(frame->rax), "=m"(frame->rbx), "=m"(frame->rcx),
          "=m"(frame->rdx), "=m"(frame->rsi), "=m"(frame->rdi),
          "=m"(frame->rbp), "=m"(frame->rsp),
          "=m"(frame->r8),  "=m"(frame->r9),
          "=m"(frame->r10), "=m"(frame->r11),
          "=m"(frame->r12), "=m"(frame->r13),
          "=m"(frame->r14), "=m"(frame->r15)
        :
        : "memory"
    );

    /* Capture current RIP — use the return address (__builtin_return_address
     * gives the caller of build_synthetic_frame, which is mce_inject_trigger,
     * which is close enough for diagnostic purposes). */
    frame->rip = (uint64_t)(uintptr_t)__builtin_return_address(0);

    /* Set CS/SS/RFLAGS to reasonable kernel-mode values.
     * In a real #MC, the CPU pushes these from the interrupted context.
     * Since we are calling from kernel context, we use kernel segments. */
    frame->cs = 0x08;    /* Kernel CS (GDT index 1) */
    frame->ss = 0x10;    /* Kernel SS (GDT index 2) */

    /* Read current RFLAGS */
    uint64_t rflags = 0;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags) :: "memory");
    frame->rflags = rflags;
}

static int mce_inject_trigger(const char *buf, int len)
{
    (void)buf;
    (void)len;

    /* Step 1: Validate inputs */
    if (g_inject_status == 0) {
        kprintf("[MCE_INJECT] ERROR: status is 0 — set a valid MCi_STATUS "
                "first (must have VAL bit 63 set)\\n");
        return -1;
    }

    if (!(g_inject_status & MC_STATUS_VAL)) {
        kprintf("[MCE_INJECT] ERROR: MCi_STATUS must have VAL bit (bit 63) set\\n");
        return -1;
    }

    /* Step 2: Validate bank number against hardware */
    uint64_t mcg_cap = read_msr(MSR_IA32_MCG_CAP);
    int num_banks = (int)(mcg_cap & MCG_CAP_COUNT_MASK);
    if (num_banks <= 0) {
        kprintf("[MCE_INJECT] ERROR: No MCA banks reported (MCG_CAP=0x%llx)\\n",
                (unsigned long long)mcg_cap);
        return -1;
    }
    if ((int)g_inject_bank >= num_banks) {
        kprintf("[MCE_INJECT] ERROR: Bank %u out of range (0..%d)\\n",
                (unsigned int)g_inject_bank, num_banks - 1);
        return -1;
    }

    kprintf("[MCE_INJECT] Injecting MCE on CPU %u bank %u: "
            "STATUS=0x%016llx ADDR=0x%016llx MISC=0x%016llx\\n",
            smp_get_cpu_id(),
            (unsigned int)g_inject_bank,
            (unsigned long long)g_inject_status,
            (unsigned long long)g_inject_addr,
            (unsigned long long)g_inject_misc);

    /* Step 3: Write values to the target bank's MSRs.
     *
     * IMPORTANT SAFETY NOTE:
     * Writing to MCi_STATUS with the VAL bit set may cause the CPU to
     * signal a machine check immediately on some microarchitectures.
     * We therefore set up the frame FIRST, then write STATUS LAST.
     *
     * On most x86-64 implementations, writing MCi_STATUS with VAL=1 and
     * MCG_STATUS.MCIP=0 will cause the CPU to take #MC immediately upon
     * the next instruction that is not a serialising instruction (or on
     * some CPUs, immediately).  To avoid re-entrance, we set MCIP first,
     * then write the per-bank MSRs, then call the handler.
     */

    /* Step 3a: Set MCG_STATUS.MCIP to signal "MCE in progress" */
    uint64_t old_mcg_status = read_msr(MSR_IA32_MCG_STATUS);
    write_msr(MSR_IA32_MCG_STATUS, old_mcg_status | MCG_STATUS_MCIP);

    /* Step 3b: Write ADDR and MISC (only if their valid bits are set) */
    if (g_inject_status & MC_STATUS_ADDRV) {
        write_msr(MSR_IA32_MC0_ADDR + 4ULL * g_inject_bank, g_inject_addr);
    }
    if (g_inject_status & MC_STATUS_MISCV) {
        write_msr(MSR_IA32_MC0_MISC + 4ULL * g_inject_bank, g_inject_misc);
    }

    /* Step 3c: Write STATUS last (this is what the CPU checks for VAL) */
    write_msr(MSR_IA32_MC0_STATUS + 4ULL * g_inject_bank, g_inject_status);

    /* Step 4: Build and call the MCE handler with a synthetic frame */
    struct interrupt_frame frame;
    build_synthetic_frame(&frame);

    kprintf("[MCE_INJECT] Calling mce_handler() with synthetic frame "
            "(RIP=0x%lx RSP=0x%lx)\\n",
            (unsigned long)frame.rip, (unsigned long)frame.rsp);

    mce_handler(&frame);

    /* Step 5: Cleanup — clear injected bank MSRs and restore MCG_STATUS.
     *
     * After mce_handler returns, it should have cleared MCIP (if it
     * determined the error was recoverable).  However, in some cases
     * (e.g. fatal injection), the handler might panic — in which case
     * we never reach here.  For non-fatal injections, we clean up. */
    write_msr(MSR_IA32_MC0_STATUS + 4ULL * g_inject_bank, 0ULL);
    write_msr(MSR_IA32_MC0_ADDR  + 4ULL * g_inject_bank, 0ULL);
    write_msr(MSR_IA32_MC0_MISC  + 4ULL * g_inject_bank, 0ULL);

    /* Restore MCG_STATUS — clear MCIP if the handler didn't already */
    uint64_t cur_mcg_status = read_msr(MSR_IA32_MCG_STATUS);
    write_msr(MSR_IA32_MCG_STATUS, cur_mcg_status & ~MCG_STATUS_MCIP);

    g_inject_count++;
    kprintf("[MCE_INJECT] Injection complete (count=%u)\\n",
            (unsigned int)g_inject_count);

    return 0;
}

/* ── Initialisation ───────────────────────────────────────────────── */

void mce_inject_init(void)
{
    /* Create the debugfs directory: /sys/kernel/debug/mce-inject/
     * Since debugfs uses a flat namespace, we prefix all entries with
     * "mce-inject/<name>".  The first call creates the directory entry,
     * subsequent calls add files. */

    /* Register parameter files */
    debugfs_create_rw_file("mce-inject/bank",
                           mce_inject_bank_read,
                           mce_inject_bank_write);
    debugfs_create_rw_file("mce-inject/status",
                           mce_inject_status_read,
                           mce_inject_status_write);
    debugfs_create_rw_file("mce-inject/addr",
                           mce_inject_addr_read,
                           mce_inject_addr_write);
    debugfs_create_rw_file("mce-inject/misc",
                           mce_inject_misc_read,
                           mce_inject_misc_write);

    /* Trigger file — write-only (read returns count) */
    debugfs_create_rw_file("mce-inject/trigger",
                           mce_inject_count_read,
                           mce_inject_trigger);

    kprintf("[MCE_INJECT] Debug interface at /sys/kernel/debug/mce-inject/*\\n");
    kprintf("[MCE_INJECT] Usage: write bank, status, [addr], [misc], then "
            "echo 1 > trigger\\n");
}
