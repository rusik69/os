#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "perf_events.h"
#include "cpu.h"
#include "heap.h"
#include "string.h"
#include "smp.h"
#include "errno.h"
#include "kallsyms.h"
#include "timer.h"
#include "sysctl.h"
#include "audit.h"
#include "process.h"
#include "caps.h"

/* Software event counters */
static struct perf_sw_counters sw_counters;

/*
 * perf_event_paranoid — sysctl kernel.perf_event_paranoid
 *
 * Controls access to performance monitoring hardware:
 *   -1: unrestricted (allow all userspace perf)
 *    0: allow CPU-level monitoring only
 *    1: disallow kernel tracepoints (default)
 *    2: disallow all userspace perf
 */
static int perf_event_paranoid = 1;

/* Sysctl read handler for perf_event_paranoid */
static int sysctl_read_perf_paranoid(char *buf, int max)
{
    char tmp[16];
    int n = 0;
    int v = perf_event_paranoid;
    if (v < 0) { if (n < max - 1) buf[n++] = '-'; v = -v; }
    if (v == 0) { if (n < max - 1) buf[n++] = '0'; }
    else {
        char rev[8];
        int rn = 0;
        while (v > 0) { rev[rn++] = (char)('0' + v % 10); v /= 10; }
        while (rn > 0 && n < max - 1) buf[n++] = rev[--rn];
    }
    if (n < max - 1) buf[n++] = '\n';
    return n;
}

/* Sysctl write handler for perf_event_paranoid */
static int sysctl_write_perf_paranoid(const char *buf, int len)
{
    int val = 0;
    int sign = 1;
    int i = 0;
    if (i < len && buf[i] == '-') { sign = -1; i++; }
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        val = val * 10 + (buf[i] - '0');
        i++;
    }
    val *= sign;
    if (val < -1 || val > 2)
        return -EINVAL;
    perf_event_paranoid = val;
    return 0;
}

/* Check if the current process is allowed to access perf events.
 * Returns 0 if allowed, -EPERM if denied.
 *
 * Levels:
 *   2: block all userspace perf
 *   1: block kernel tracepoints (not yet implemented separately)
 *   0: allow CPU-level monitoring (counting), block privileged
 *  -1: allow everything
 */
int perf_paranoid_check(void)
{
    struct process *p = process_get_current();

    /* Kernel context always allowed */
    if (!p || !p->is_user)
        return 0;

    if (perf_event_paranoid >= 2) {
        /* Level 2: everything blocked for userspace */
        audit_log_denial("perf_events", "perf_event_open", "level2_block");
        return -EPERM;
    }

    if (perf_event_paranoid == 1) {
        /* Level 1: userspace can do CPU-level monitoring but not
         * kernel tracepoints. Since we only have counters (no tracepoints
         * implemented yet), allow through. */
        /* TODO: when tracepoints are implemented, check the event type here */
    }

    if (perf_event_paranoid == 0) {
        /* Level 0: allow CPU-level monitoring if process has
         * CAP_SYS_ADMIN or CAP_SYS_PTRACE, otherwise still allow
         * but log a warning */
        /* For now, allow everything at level 0 */
    }

    /* Level -1: unrestricted */
    return 0;
}

/* Initialize perf_event_paranoid sysctl */
void perf_paranoid_sysctl_init(void)
{
    sysctl_register("perf_event_paranoid",
                    sysctl_read_perf_paranoid,
                    sysctl_write_perf_paranoid);
    kprintf("[OK] perf_event_paranoid=%d sysctl registered\n", perf_event_paranoid);
}

/* Check if the CPU supports architectural PMU */
static int perf_available = 0;

/* ── PEBS per-CPU state ───────────────────────────────────────────────── */
static struct pebs_cpu_state pebs_state[SMP_MAX_CPUS];

/* PEBS platform availability (CPUID leaf 0x01, EDX bit 21 = DS) */
static int pebs_platform_ok = 0;

/* Read a performance counter register */
uint64_t perf_read_pmc(int counter)
{
    uint64_t val = 0;

    if (!perf_available)
        return 0;

    switch (counter) {
    case 0:
        val = read_msr(IA32_PMC0);
        break;
    case 1:
        val = read_msr(IA32_PMC1);
        break;
    default:
        /* PMC2 and PMC3 are model-specific; try to read sequentially */
        if (counter >= 0 && counter < 8)
            val = read_msr(IA32_PMC0 + counter);
        break;
    }
    return val;
}

/* Enable performance monitoring globally */
void perf_enable(void)
{
    if (!perf_available)
        return;
    write_msr(IA32_PERF_GLOBAL_CTRL, 0x70000000fULL);
}

/* Disable performance monitoring globally */
void perf_disable(void)
{
    if (!perf_available)
        return;
    write_msr(IA32_PERF_GLOBAL_CTRL, 0);
}

/* Configure a generic performance counter to count a specific event */
void perf_configure_event(int counter, uint64_t event_sel)
{
    if (!perf_available || counter < 0 || counter > 3)
        return;
    write_msr(IA32_PERFEVTSEL0 + counter, event_sel);
}

/* Software event: context switch */
void perf_sw_context_switch(void)
{
    sw_counters.context_switches++;
}

/* Software event: page fault */
void perf_sw_page_fault(void)
{
    sw_counters.page_faults++;
}

/* Read software counters */
uint64_t perf_sw_read_context_switches(void)
{
    return sw_counters.context_switches;
}

uint64_t perf_sw_read_page_faults(void)
{
    return sw_counters.page_faults;
}

/* Set up a basic cycle counter on PMC0 */
static void perf_setup_basic_counters(void)
{
    /* IA32_PERFEVTSEL0: Count CPU cycles at all privilege levels (event 0x3C, umask 0x00) */
    uint64_t sel0 = (0x3CULL << 0) |  /* Event Select: CPU_CLK_UNHALTED.CORE */
                    (0x00ULL << 8) |  /* Umask */
                    (1ULL << 16) |    /* USR (user mode) */
                    (1ULL << 17) |    /* OS (kernel mode) */
                    (1ULL << 18) |    /* E (enable) */
                    (1ULL << 22);     /* EN (any thread) */
    write_msr(IA32_PERFEVTSEL0, sel0);

    /* IA32_PERFEVTSEL1: Count instructions retired (event 0xC0, umask 0x00) */
    uint64_t sel1 = (0xC0ULL << 0) |  /* Event Select: INST_RETIRED.ANY */
                    (0x00ULL << 8) |  /* Umask */
                    (1ULL << 16) |    /* USR */
                    (1ULL << 17) |    /* OS */
                    (1ULL << 18);     /* E (enable) */
    write_msr(IA32_PERFEVTSEL0 + 1, sel1);

    /* Clear the counters */
    write_msr(IA32_PMC0, 0);
    write_msr(IA32_PMC1, 0);

    /* Enable counting */
    perf_enable();
}

/* ══════════════════════════════════════════════════════════════════════════
 * PEBS — Precise Event-Based Sampling
 * ══════════════════════════════════════════════════════════════════════════ */

int pebs_available(void)
{
    return pebs_platform_ok;
}

/* Initialise PEBS / DS area for the current CPU. */
int pebs_init(void)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return -ENODEV;

    /* Already initialised on this CPU */
    if (pebs_state[cpu].initialized)
        return 0;

    if (!pebs_platform_ok)
        return -ENODEV;

    /* Allocate the Debug Store area (4KB, aligned to 64 bytes).
     * The CPU writes to this area via the DS mechanism — it must reside
     * in memory that stays mapped and cacheable.  kmalloc is fine. */
    struct debug_store *ds = (struct debug_store *)kmalloc(4096);
    if (!ds)
        return -ENOMEM;
    memset(ds, 0, 4096);

    /* Allocate the PEBS sample buffer (4KB, aligned to 64 bytes). */
    struct pebs_record *buf = (struct pebs_record *)kmalloc(PEBS_BUFFER_SIZE);
    if (!buf) {
        kfree(ds);
        return -ENOMEM;
    }
    memset(buf, 0, PEBS_BUFFER_SIZE);

    /* Configure the DS area:
     *   - BTS fields stay NULL (disabled)
     *   - PEBS buffer base = start of our buffer
     *   - PEBS index = same as base (empty)
     *   - PEBS absolute maximum = end of buffer
     *   - PEBS interrupt threshold = 3/4 full (generate PMI before overflow)
     *   - Counter reset values = 0 (no auto-reload by default) */
    ds->pebs_buffer_base         = (uint64_t)(uintptr_t)buf;
    ds->pebs_index               = (uint64_t)(uintptr_t)buf;
    ds->pebs_absolute_maximum    = (uint64_t)(uintptr_t)buf + PEBS_BUFFER_SIZE;
    ds->pebs_interrupt_threshold = (uint64_t)(uintptr_t)buf +
                                   (PEBS_BUFFER_SIZE * 3 / 4);

    /* Write the DS area base address into IA32_DS_AREA */
    write_msr(IA32_DS_AREA, (uint64_t)(uintptr_t)ds);

    /* Record state */
    pebs_state[cpu].ds_area         = ds;
    pebs_state[cpu].pebs_buffer     = buf;
    pebs_state[cpu].pebs_counter    = -1;
    pebs_state[cpu].sample_count    = 0;
    pebs_state[cpu].initialized     = 1;

    kprintf("[OK] pebs: CPU%d DS area @ va 0x%llx, PEBS buffer @ va 0x%llx (%d records)\n",
            cpu, (unsigned long long)(uintptr_t)ds,
            (unsigned long long)(uintptr_t)buf, PEBS_MAX_RECORDS);

    return 0;
}

/* Enable PEBS sampling on a specific counter.
 *
 * Counter configuration:
 *   - The event_sel value has INT (bit 20) forced on so that an
 *     interrupt is generated on counter overflow.
 *   - The EN bit (bit 22) is forced on to enable counting on all
 *     threads (hyperthreading).
 *   - The counter is written with reset_val so that after each sample
 *     the hardware reloads it from IA32_DS_AREA's pebs_counter_reset[counter].
 *
 * PEBS enable:
 *   - IA32_PEBS_ENABLE[counter] = 1  enables PEBS for that counter.
 *   - When the counter overflows, the CPU writes a PEBS record to
 *     the buffer instead of immediately taking a PMI.
 *   - The CPU then resets the counter to ds->pebs_counter_reset[counter]
 *     and continues counting.
 *   - When the PEBS index reaches the interrupt threshold, a PMI is
 *     raised and the handler must drain the buffer. */
int pebs_enable_counter(int counter, uint64_t event_sel, uint64_t reset_val)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return -ENODEV;

    if (!pebs_state[cpu].initialized)
        return -ENXIO;

    if (counter < 0 || counter >= PEBS_MAX_COUNTERS)
        return -EINVAL;

    /* Disable performance counting while we reconfigure */
    perf_disable();

    /* Set the counter reset value in the DS area (auto-reload on sample) */
    pebs_state[cpu].ds_area->pebs_counter_reset[counter] = reset_val;
    pebs_state[cpu].pebs_counter_reset = reset_val;

    /* Configure the event selector with INT forced on.
     * The caller provides base event+umask; we add control bits. */
    uint64_t sel = event_sel |
                   PERFEVTSEL_ENABLE |
                   PERFEVTSEL_INT |
                   PERFEVTSEL_USR |
                   PERFEVTSEL_OS;
    write_msr(IA32_PERFEVTSEL0 + counter, sel);

    /* Load the counter with the reset value (so it counts from here) */
    write_msr(IA32_PMC0 + counter, reset_val);

    /* Enable PEBS for this counter via IA32_PEBS_ENABLE */
    uint64_t pebs_enable = read_msr(IA32_PEBS_ENABLE);
    pebs_enable |= (1ULL << counter);
    write_msr(IA32_PEBS_ENABLE, pebs_enable);

    /* Record which counter has PEBS on this CPU */
    pebs_state[cpu].pebs_counter = counter;

    /* Re-enable performance counting */
    perf_enable();

    kprintf("[OK] pebs: CPU%d counter%d enabled (event_sel=0x%llx, reset=0x%llx)\n",
            cpu, counter, (unsigned long long)event_sel,
            (unsigned long long)reset_val);

    return 0;
}

/* Disable PEBS sampling on the given counter. */
void pebs_disable_counter(int counter)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;

    if (!pebs_state[cpu].initialized)
        return;

    if (counter < 0 || counter >= PEBS_MAX_COUNTERS)
        return;

    perf_disable();

    /* Clear the PEBS enable bit for this counter */
    uint64_t pebs_enable = read_msr(IA32_PEBS_ENABLE);
    pebs_enable &= ~(1ULL << counter);
    write_msr(IA32_PEBS_ENABLE, pebs_enable);

    /* Disable the counter */
    write_msr(IA32_PERFEVTSEL0 + counter, 0);

    /* Clear the RCX (counter) */
    write_msr(IA32_PMC0 + counter, 0);

    /* Reset state if this was our PEBS counter */
    if (pebs_state[cpu].pebs_counter == counter)
        pebs_state[cpu].pebs_counter = -1;

    perf_enable();

    kprintf("[OK] pebs: CPU%d counter%d disabled\n", cpu, counter);
}

/* Drain the PEBS buffer: read all available records.
 *
 * The CPU writes PEBS records into the buffer and advances the PEBS index.
 * Records are valid from pebs_buffer_base up to pebs_index.
 * After reading, we reset the index back to the base (logical drain).
 *
 * Returns the number of records copied, or 0 if none available. */
int pebs_read_samples(struct pebs_record *buf, int max_count)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return 0;

    if (!pebs_state[cpu].initialized || !buf || max_count <= 0)
        return 0;

    struct debug_store *ds = pebs_state[cpu].ds_area;
    uint64_t base   = ds->pebs_buffer_base;
    uint64_t index  = ds->pebs_index;
    uint64_t count  = (index - base) / PEBS_RECORD_SIZE;

    if (count == 0)
        return 0;

    /* Clamp to caller's max and available buffer capacity */
    int n = (int)count;
    if (n > max_count)
        n = max_count;
    if (n > PEBS_MAX_RECORDS)
        n = PEBS_MAX_RECORDS;

    /* Copy records from the PEBS buffer to caller buffer */
    struct pebs_record *src = pebs_state[cpu].pebs_buffer;
    for (int i = 0; i < n; i++) {
        buf[i] = src[i];
    }

    /* Reset the PEBS index back to the buffer base (drain).
     * The CPU will start writing new records from the base again.
     * We must also clear the GLOBAL_STATUS_DS_BUFFER bit if it was set. */
    ds->pebs_index = base;

    /* Acknowledge the DS buffer overflow interrupt by writing
     * GLOBAL_STATUS_DS_BUFFER to IA32_PERF_GLOBAL_OVF_CTRL */
    uint64_t ovf_status = read_msr(IA32_PERF_GLOBAL_STATUS);
    if (ovf_status & GLOBAL_STATUS_DS_BUFFER) {
        write_msr(IA32_PERF_GLOBAL_OVF_CTRL, GLOBAL_STATUS_DS_BUFFER);
    }

    pebs_state[cpu].sample_count += n;

    return n;
}

/* Return total number of PEBS samples collected so far. */
int pebs_total_samples(void)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return 0;
    return pebs_state[cpu].sample_count;
}

/* ══════════════════════════════════════════════════════════════════════════
 * LBR — Last Branch Record Stack
 * ══════════════════════════════════════════════════════════════════════════ */

/* LBR state: depth detected at init, and whether we have architectural LBR */
static int lbr_available_depth = 0;  /* 0 = not available */
static int lbr_arch_supported = 0;   /* 1 = architectural LBR (Ice Lake+) */
static int lbr_enabled_on_cpu[SMP_MAX_CPUS];

/* Detect the LBR depth by probing CPUID leaf 0x1C (architectural LBR)
 * or falling back to the legacy Nehalem-era default of 16. */
int lbr_detect_depth(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Check for architectural LBR via CPUID leaf 0x1C */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x1C), "c"(0));

    if (eax > 0) {
        /* Leaf 0x1C valid — architectural LBR is supported.
         * eax[27:25]  = LBR depth encoding (0=8, 1=16, 2=32)
         * ebx[15:0]   = supported depth mask
         * ebx[31:16]  = CPL filter, branch filter support etc. */
        (void)edx;

        int depth_val = (eax >> 25) & 0x7;
        int depth = 0;
        switch (depth_val) {
            case 0:  depth = 8;  break;
            case 1:  depth = 16; break;
            case 2:  depth = 32; break;
            default: depth = 16; /* safe fallback */
        }

        lbr_arch_supported = 1;
        kprintf("[OK] lbr: architectural LBR depth=%d (CPUID.0x1C)\n", depth);
        return depth;
    }

    /* Legacy LBR: probe MSR_LBR_SELECT to see if LBRv3 filtering is present.
     * If the MSR reads back the written value, it's supported.
     * Safe default for Nehalem+: 16 LBR entries (MSR 0x680-0x68F / 0x6C0-0x6CF). */
    uint64_t probe_val = LBR_SELECT_CALL | LBR_SELECT_IND_CALL;
    write_msr(MSR_LBR_SELECT, probe_val);
    uint64_t readback = read_msr(MSR_LBR_SELECT);

    if (readback == probe_val) {
        /* MSR_LBR_SELECT works — we have at least LBRv3 with 32 entries */
        lbr_arch_supported = 0;
        kprintf("[OK] lbr: legacy LBRv3 depth=32 (MSR_LBR_SELECT response)\n");
        /* Reset the filter to capture everything */
        write_msr(MSR_LBR_SELECT, 0);
        return 32;
    }

    /* Fallback: probe the first FROM MSR to see if LBR exists at all */
    uint64_t from0 = read_msr(MSR_LBR_NHM_FROM(0));
    write_msr(MSR_LBR_NHM_FROM(0), ~0ULL);
    uint64_t from0_written = read_msr(MSR_LBR_NHM_FROM(0));
    write_msr(MSR_LBR_NHM_FROM(0), from0); /* restore */

    if (from0_written == ~0ULL) {
        lbr_arch_supported = 0;
        kprintf("[OK] lbr: legacy LBR depth=16 (MSR probe)\n");
        return 16;
    }

    /* No LBR support detected */
    kprintf("[OK] lbr: not available\n");
    return 0;
}

/* Return the detected LBR depth (public API) */
int lbr_depth(void)
{
    return lbr_available_depth;
}

/* Enable LBR recording on the current CPU. */
void lbr_enable(uint64_t flags)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;

    if (lbr_available_depth <= 0)
        return;  /* LBR not available */

    if (lbr_arch_supported) {
        /* Architectural LBR: program MSR_ARCH_LBR_CTL */
        uint64_t ctl = ARCH_LBR_CTL_LBR_EN;

        /* Set depth encoding in bits 8-11 */
        int depth_code = 0;
        if (lbr_available_depth == 8)       depth_code = 0;
        else if (lbr_available_depth == 16) depth_code = 1;
        else if (lbr_available_depth == 32) depth_code = 2;
        ctl |= (uint64_t)(depth_code & 0xF) << ARCH_LBR_CTL_DEPTH_SHIFT;

        /* Apply filtering flags (branch type filter in bits 1-7) */
        ctl |= (flags & 0x7F) << ARCH_LBR_CTL_FILTER_SHIFT;

        write_msr(MSR_ARCH_LBR_CTL, ctl);
    } else {
        /* Legacy LBR: write IA32_DEBUGCTL with LBR bit */
        uint64_t debugctl = read_msr(IA32_DEBUGCTL);
        debugctl |= IA32_DEBUGCTL_LBR;

        /* Apply filter via MSR_LBR_SELECT if it was detected */
        write_msr(IA32_DEBUGCTL, debugctl);
        write_msr(MSR_LBR_SELECT, flags);
    }

    lbr_enabled_on_cpu[cpu] = 1;
}

/* Disable LBR recording on the current CPU. */
void lbr_disable(void)
{
    int cpu = smp_get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;

    if (lbr_arch_supported) {
        write_msr(MSR_ARCH_LBR_CTL, 0);
    } else {
        uint64_t debugctl = read_msr(IA32_DEBUGCTL);
        debugctl &= ~IA32_DEBUGCTL_LBR;
        write_msr(IA32_DEBUGCTL, debugctl);
    }

    lbr_enabled_on_cpu[cpu] = 0;
}

/* Read the current LBR stack into the caller's buffer.
 * Returns the number of valid entries (0 if LBR is disabled or empty).
 * LBR entries are read from newest to oldest (index 0 = most recent branch). */
int lbr_read(struct lbr_entry *entries)
{
    if (!entries || lbr_available_depth <= 0)
        return 0;

    int depth = lbr_available_depth;
    if (depth > LBR_MAX_DEPTH)
        depth = LBR_MAX_DEPTH;

    if (lbr_arch_supported) {
        /* Architectural LBR: read FROM/TO from the MSR bank */
        for (int i = 0; i < depth; i++) {
            entries[i].from = read_msr(MSR_ARCH_LBR_FROM_BASE + i);
            entries[i].to   = read_msr(MSR_ARCH_LBR_TO_BASE + i);
            /* Attempt to read optional LBR_INFO MSR */
            entries[i].info = read_msr(MSR_ARCH_LBR_INFO_BASE + i);
        }
    } else {
        /* Legacy Nehalem LBR: read FROM/TO MSR pairs */
        for (int i = 0; i < depth; i++) {
            entries[i].from = read_msr(MSR_LBR_NHM_FROM(i));
            entries[i].to   = read_msr(MSR_LBR_NHM_TO(i));
            entries[i].info = 0;  /* no architectural info on legacy LBR */
        }
    }

    return depth;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Topdown Metrics — Pipeline Slot Breakdown (Ice Lake+)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Topdown Metrics categorises each CPU pipeline slot into one of four
 * classes: Frontend Bound, Bad Speculation, Backend Bound, and Retiring.
 * The IA32_PERF_METRICS MSR (0x329) provides this breakdown as U2.16
 * fixed-point fractions of total pipeline slots.  Fixed counter 2 must
 * be configured to count TOPDOWN.SLOTS and the metrics MSR must be
 * enabled in IA32_PERF_GLOBAL_CTRL[12].
 *
 * Reference: Intel® 64 and IA-32 Architectures SDM Vol. 3B, Ch. 20.5.
 * ══════════════════════════════════════════════════════════════════════════ */
/* ── Topdown Metrics (Item 207) ──────────────────────────────────────── */

static int g_topdown_available = 0;
static int g_topdown_configured = 0;  /* per-CPU: has topdown been enabled */

/* ── Forward declarations for multiplexing ─────────────────────────── */
void perf_mux_init(void);

/* Check Topdown Metrics availability via CPUID leaf 0x0A, ECX bit 15.
 * This bit is set on Ice Lake and later processors (both client and server)
 * that support the Topdown Metrics infrastructure. */
int topdown_available(void)
{
    return g_topdown_available;
}

/* ── Topdown Metrics enable ──────────────────────────────────────────── */

int topdown_enable(void)
{
    if (!g_topdown_available)
        return -ENODEV;

    /* IA32_PERF_GLOBAL_CTRL must be saved/restored carefully to avoid
     * interfering with other counters.  We just OR in the new bits. */
    uint64_t gctrl = read_msr(IA32_PERF_GLOBAL_CTRL);

    /* Step 1: Enable the IA32_PERF_METRICS MSR (bit 12) so the hardware
     * populates it on each cycle. */
    gctrl |= (1ULL << 12);
    write_msr(IA32_PERF_GLOBAL_CTRL, gctrl);

    /* Step 2: Configure fixed counter 2 control register.
     * IA32_FIXED_CTR_CTRL is at MSR 0x38D; FC2 control lives in bits 16-23.
     *
     * We program FC2 to count TOPDOWN.SLOTS (the architected event for
     * fixed counter 2).  The event encoding is implicit for fixed counters:
     *   - FC0 counts INST_RETIRED.ANY
     *   - FC1 counts CPU_CLK_UNHALTED.THREAD
     *   - FC2 counts TOPDOWN.SLOTS (when bit 16 is set)
     *
     * We count at all privilege levels (OS + USR) but NOT on any thread
     * (we want per-core, not per-thread, for the metrics to be meaningful). */
    uint64_t fc_ctrl = read_msr(IA32_FIXED_CTR_CTRL);

    /* Mask off old FC2 control bits (16-23) */
    fc_ctrl &= ~(0xFFULL << 16);
    /* Set new FC2 control: enable + kernel + user, no PMI */
    fc_ctrl |= FIXED_CTR2_CTRL_EN     |
               FIXED_CTR2_CTRL_KERNEL |
               FIXED_CTR2_CTRL_USER;
    write_msr(IA32_FIXED_CTR_CTRL, fc_ctrl);

    /* Step 3: Clear the fixed counter and ensure it is running.
     * If IA32_PERF_GLOBAL_CTRL bit 34 is 0, set it to enable FC2. */
    gctrl |= (1ULL << 34);             /* Enable FC2 */
    write_msr(IA32_PERF_GLOBAL_CTRL, gctrl);

    /* Clear fixed counter 2 so we start from zero */
    write_msr(IA32_FIXED_CTR2, 0);  /* FC2 at MSR 0x30B */

    g_topdown_configured = 1;

    kprintf("[OK] topdown: enabled on CPU%d (PERF_METRICS MSR + FC2)\\n",
            smp_get_cpu_id());

    return 0;
}

/* ── Read Topdown Metrics ───────────────────────────────────────────── */

int topdown_read(struct topdown_metrics *metrics)
{
    if (!metrics)
        return -EINVAL;

    if (!g_topdown_available || !g_topdown_configured)
        return -ENODEV;

    /* Read the IA32_PERF_METRICS MSR (0x329) which contains the four
     * slot fractions in U2.16 fixed-point format.
     *
     * MSR layout (Ice Lake+):
     *   Bits 15:0   = FRONTEND_BOUND  (U2.16 fraction of total slots)
     *   Bits 31:16  = BAD_SPECULATION (U2.16)
     *   Bits 47:32  = BACKEND_BOUND   (U2.16)
     *   Bits 63:48  = RETIRING        (U2.16)
     *
     * The sum of all four should be approximately 1.0 (since each cycle
     * provides a fixed number of pipeline slots; on modern Intel cores
     * this is typically 4-6 slots per cycle).  A value of 0x10000 = 1.0
     * in U2.16. */
    uint64_t raw = read_msr(IA32_PERF_METRICS);

    metrics->frontend_bound  = (uint32_t)(raw & 0xFFFF);
    metrics->bad_speculation = (uint32_t)((raw >> 16) & 0xFFFF);
    metrics->backend_bound   = (uint32_t)((raw >> 32) & 0xFFFF);
    metrics->retiring        = (uint32_t)((raw >> 48) & 0xFFFF);

    return 0;
}

/* ── Disable Topdown ────────────────────────────────────────────────── */

void topdown_disable(void)
{
    if (!g_topdown_configured)
        return;

    /* Disable fixed counter 2 */
    uint64_t fc_ctrl = read_msr(IA32_FIXED_CTR_CTRL);
    fc_ctrl &= ~(FIXED_CTR2_CTRL_EN << 16);
    write_msr(IA32_FIXED_CTR_CTRL, fc_ctrl);

    /* Clear the metrics enable bit */
    uint64_t gctrl = read_msr(IA32_PERF_GLOBAL_CTRL);
    gctrl &= ~(1ULL << 34);   /* Disable FC2 */
    gctrl &= ~(1ULL << 12);   /* Disable PERF_METRICS */
    write_msr(IA32_PERF_GLOBAL_CTRL, gctrl);

    g_topdown_configured = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Topdown-level Initialisation (called from perf_init)
 * ══════════════════════════════════════════════════════════════════════════ */

static void topdown_init(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Check Topdown support: CPUID leaf 0x0A, sub-leaf 0, ECX bit 15.
     * This is the "Topdown Metrics" availability bit. */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x0a), "c"(0));

    if (ecx & (1U << 15)) {
        g_topdown_available = 1;
        kprintf("[OK] topdown: IA32_PERF_METRICS supported (Ice Lake+)\\n");
    } else {
        g_topdown_available = 0;
        kprintf("[OK] topdown: not supported by this CPU\\n");
    }

    g_topdown_configured = 0;
}

void perf_init(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* ── Check for architectural PMU via CPUID leaf 0x0A ── */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x0a), "c"(0));

    if ((eax & 0xFF) > 0) {
        perf_available = 1;
        sw_counters.context_switches = 0;
        sw_counters.page_faults = 0;
        sw_counters.cpu_cycles = 0;
        sw_counters.instructions = 0;

        /* Verify MSR access works by reading the global control MSR */
        uint64_t gctrl = read_msr(IA32_PERF_GLOBAL_CTRL);
        (void)gctrl;

        perf_setup_basic_counters();

        kprintf("[OK] perf_events: %d PMC counters available, version %d\n",
                (int)(eax >> 8) & 0xFF, (int)(eax & 0xFF));
    } else {
        perf_available = 0;
        kprintf("[OK] perf_events: no hardware PMU, software counters only\n");
    }

    /* ── Check PEBS availability via CPUID leaf 0x01, EDX[21] = DS ── */
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x01), "c"(0));

    if (edx & (1U << 21)) {
        pebs_platform_ok = 1;
        kprintf("[OK] perf_events: PEBS / Debug Store supported\n");
    } else {
        pebs_platform_ok = 0;
        kprintf("[OK] perf_events: PEBS not available (no DS in CPUID)\n");
    }

    /* ── Detect LBR capability ── */
    lbr_available_depth = lbr_detect_depth();
    if (lbr_available_depth > 0) {
        kprintf("[OK] lbr: %d entries, %s mode\n",
                lbr_available_depth,
                lbr_arch_supported ? "architectural" : "legacy");
    } else {
        kprintf("[OK] lbr: not available on this CPU\n");
    }

    /* Reset per-CPU LBR enabled state */
    for (int i = 0; i < SMP_MAX_CPUS; i++)
        lbr_enabled_on_cpu[i] = 0;

    /* ── Initialise Topdown Metrics (Item 207) ── */
    topdown_init();

    /* ── Initialise hardware counter multiplexing ── */
    perf_mux_init();

    /* ── Initialise perf_event_paranoid sysctl ── */
    perf_paranoid_sysctl_init();
}

/* ══════════════════════════════════════════════════════════════════════════
 * Hardware PMC Multiplexing (S131)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * When there are more active events than available PMCs, we time-share
 * the physical counters among the events.  On each timer tick, we rotate
 * which events are currently assigned to PMCs.  The accumulated counts
 * for each event are scaled proportionally to account for the rotated-out
 * time.
 *
 * State:
 *   - mux_events[]: array of all active events (max MUX_MAX_EVENTS)
 *   - mux_num_events: number of registered events
 *   - mux_tick_count: counter of ticks since the last reset
 *   - mux_enabled_flag: 1 = muxing is active, 0 = pass-through
 *   - mux_interval: number of ticks each event group stays on the PMCs
 *
 * Each event stores:
 *   - raw_count: value last read from the hardware counter
 *   - scaled_count: accumulated count scaled by (total_events / num_pmcs)
 *   - event_config: IA32_PERFEVTSEL value for this event
 *
 * On each rotation:
 *   1. Read current PMC values and accumulate scaled counts for rotated-out events
 *   2. Write new event configs for the next group of events
 *   3. Clear the new PMCs
 */

#define MUX_MAX_EVENTS    16
#define MUX_NUM_PMCS      4   /* PMC0-PMC3 */
#define MUX_DEFAULT_INTERVAL 10  /* ticks before rotating */

struct mux_event {
    uint64_t event_config;   /* PERFEVTSEL value (without enable bit stored here) */
    uint64_t raw_count;      /* last raw PMU value read */
    uint64_t scaled_count;   /* accumulated (scaled) count */
    int      active;
};

static struct mux_event g_mux_events[MUX_MAX_EVENTS];
static int    g_mux_num_events = 0;
static int    g_mux_next_event = 0;     /* index of next event to assign to PMC0 */
static int    g_mux_interval = MUX_DEFAULT_INTERVAL;
static int    g_mux_tick = 0;           /* counts ticks since last rotation */
static int    g_mux_enabled_flag = 0;
static int    g_mux_initialized = 0;
static spinlock_t g_mux_lock;

void perf_mux_init(void)
{
    if (g_mux_initialized) return;
    memset(g_mux_events, 0, sizeof(g_mux_events));
    g_mux_num_events = 0;
    g_mux_next_event = 0;
    g_mux_tick = 0;
    g_mux_enabled_flag = 0;
    g_mux_interval = MUX_DEFAULT_INTERVAL;
    spinlock_init(&g_mux_lock);
    g_mux_initialized = 1;
    kprintf("[perf] PMC multiplexing initialized (max %d events, interval=%d ticks)\n",
            MUX_MAX_EVENTS, g_mux_interval);
}

int perf_mux_enabled(void)
{
    return g_mux_enabled_flag;
}

void perf_mux_enable(void)
{
    g_mux_enabled_flag = 1;
    g_mux_tick = 0;
    g_mux_next_event = 0;
    kprintf("[perf] MUX enabled\n");
}

void perf_mux_disable(void)
{
    g_mux_enabled_flag = 0;
    kprintf("[perf] MUX disabled\n");
}

/* Set the rotation interval (in timer ticks). */
void perf_mux_set_interval(int ticks)
{
    if (ticks < 1) ticks = 1;
    if (ticks > 1000) ticks = 1000;
    g_mux_interval = ticks;
}

/* Register an event for multiplexing.
 * Returns the event index on success, -1 on error. */
int perf_mux_register_event(uint64_t event_config)
{
    if (!g_mux_initialized) perf_mux_init();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mux_lock, &irq_flags);

    if (g_mux_num_events >= MUX_MAX_EVENTS) {
        spinlock_irqsave_release(&g_mux_lock, irq_flags);
        return -1;
    }

    int idx = g_mux_num_events;
    g_mux_events[idx].event_config = event_config;
    g_mux_events[idx].raw_count = 0;
    g_mux_events[idx].scaled_count = 0;
    g_mux_events[idx].active = 1;
    g_mux_num_events++;

    spinlock_irqsave_release(&g_mux_lock, irq_flags);
    return idx;
}

/* Unregister an event (by index returned from perf_mux_register_event). */
void perf_mux_unregister_event(int idx)
{
    if (idx < 0 || idx >= MUX_MAX_EVENTS) return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mux_lock, &irq_flags);
    g_mux_events[idx].active = 0;
    g_mux_events[idx].event_config = 0;
    spinlock_irqsave_release(&g_mux_lock, irq_flags);
}

/* Read the scaled count of an event. */
uint64_t perf_mux_read_event(int idx)
{
    if (idx < 0 || idx >= MUX_MAX_EVENTS || !g_mux_events[idx].active)
        return 0;
    return g_mux_events[idx].scaled_count;
}

/*
 * Called on each timer tick.  If multiplexing is enabled and the interval
 * has elapsed, rotate which events are assigned to the physical PMCs.
 */
void perf_mux_tick(void)
{
    if (!g_mux_enabled_flag || g_mux_num_events == 0)
        return;

    g_mux_tick++;

    if (g_mux_tick < g_mux_interval)
        return;

    /* Time to rotate */
    g_mux_tick = 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mux_lock, &irq_flags);

    int num_events = g_mux_num_events;
    int active_count = 0;
    for (int i = 0; i < num_events; i++) {
        if (g_mux_events[i].active) active_count++;
    }
    if (active_count == 0) {
        spinlock_irqsave_release(&g_mux_lock, irq_flags);
        return;
    }

    /* Determine the scale factor: if we have more events than PMCs,
     * each event gets (num_events / NUM_PMCS) times its raw count. */
    float scale = (float)active_count / (float)MUX_NUM_PMCS;
    if (scale < 1.0f) scale = 1.0f;

    /* Read current PMC values and accumulate scaled counts */
    for (int i = 0; i < MUX_NUM_PMCS; i++) {
        uint64_t cur_val = perf_read_pmc(i);
        int event_idx = (g_mux_next_event + i) % num_events;

        if (g_mux_events[event_idx].active) {
            uint64_t delta = cur_val - g_mux_events[event_idx].raw_count;
            g_mux_events[event_idx].scaled_count += (uint64_t)((float)delta * scale);
        }
        g_mux_events[event_idx].raw_count = 0;
    }

    /* Advance to the next group of events */
    g_mux_next_event = (g_mux_next_event + MUX_NUM_PMCS) % num_events;

    /* Disable PMCs during reconfiguration */
    perf_disable();

    /* Clear counters */
    for (int i = 0; i < MUX_NUM_PMCS; i++) {
        write_msr(IA32_PMC0 + i, 0);
    }

    /* Program new events into PMCs */
    for (int i = 0; i < MUX_NUM_PMCS; i++) {
        int event_idx = (g_mux_next_event + i) % num_events;
        if (g_mux_events[event_idx].active) {
            write_msr(IA32_PERFEVTSEL0 + i,
                      g_mux_events[event_idx].event_config | PERFEVTSEL_ENABLE);
        } else {
            write_msr(IA32_PERFEVTSEL0 + i, 0);
        }
    }

    /* Re-enable PMCs */
    perf_enable();

    spinlock_irqsave_release(&g_mux_lock, irq_flags);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Perf Context-Switch Tracing (Item 4)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Tracks context switches with per-process PID, TID, and timestamp
 * for later analysis.  Maintains a ring buffer of switch events.
 */

#define PERF_CSWITCH_BUF_SIZE 1024

struct perf_cswitch_event {
    uint64_t timestamp;      /* TSC or timer ticks */
    uint32_t prev_pid;       /* Previous process PID */
    uint32_t next_pid;       /* Next process PID */
    char     prev_comm[16];  /* Previous process name */
    char     next_comm[16];  /* Next process name */
    uint8_t  prev_state;     /* Previous task state */
};

static struct {
    struct perf_cswitch_event events[PERF_CSWITCH_BUF_SIZE];
    volatile uint32_t write_idx;
    int enabled;
    int initialized;
} perf_cswitch_state;

void perf_cswitch_init(void)
{
    if (perf_cswitch_state.initialized) return;
    memset(&perf_cswitch_state, 0, sizeof(perf_cswitch_state));
    perf_cswitch_state.enabled = 1;
    perf_cswitch_state.initialized = 1;
}

void perf_cswitch_enable(void)
{
    perf_cswitch_state.enabled = 1;
}

void perf_cswitch_disable(void)
{
    perf_cswitch_state.enabled = 0;
}

void perf_cswitch_trace(uint32_t prev_pid, uint32_t next_pid,
                        const char *prev_comm, const char *next_comm,
                        uint8_t prev_state)
{
    if (!perf_cswitch_state.initialized || !perf_cswitch_state.enabled)
        return;

    uint32_t idx = __sync_fetch_and_add(&perf_cswitch_state.write_idx, 1)
                   % PERF_CSWITCH_BUF_SIZE;

    struct perf_cswitch_event *ev = &perf_cswitch_state.events[idx];
    ev->timestamp = timer_get_ticks();
    ev->prev_pid = prev_pid;
    ev->next_pid = next_pid;
    ev->prev_state = prev_state;

    if (prev_comm) {
        strncpy(ev->prev_comm, prev_comm, 15);
        ev->prev_comm[15] = '\0';
    } else {
        ev->prev_comm[0] = '\0';
    }
    if (next_comm) {
        strncpy(ev->next_comm, next_comm, 15);
        ev->next_comm[15] = '\0';
    } else {
        ev->next_comm[0] = '\0';
    }
}

int perf_cswitch_read(struct perf_cswitch_event *buf, int max_count)
{
    if (!buf || max_count <= 0)
        return 0;

    uint32_t count = perf_cswitch_state.write_idx;
    if (count > PERF_CSWITCH_BUF_SIZE)
        count = PERF_CSWITCH_BUF_SIZE;
    if ((int)count > max_count)
        count = (uint32_t)max_count;

    for (uint32_t i = 0; i < count; i++) {
        buf[i] = perf_cswitch_state.events[i];
    }
    return (int)count;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Perf Page Fault Sampling (Item 5)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Samples page faults with full context: faulting address, instruction
 * pointer, process PID, and a partial stack trace.  Stores to a ring
 * buffer for later analysis (perf report, flame graphs).
 */

#define PERF_PF_BUF_SIZE 512
#define PERF_PF_STACK_DEPTH 16  /* Max stack frames to capture */

struct perf_pf_sample {
    uint64_t timestamp;       /* Timer ticks at time of fault */
    uint64_t fault_addr;      /* Address that caused the fault */
    uint64_t ip;              /* Instruction pointer at fault */
    uint64_t stack[PERF_PF_STACK_DEPTH]; /* Partial stack trace */
    uint32_t pid;             /* Process PID */
    uint32_t error_code;      /* Page fault error code */
    int      stack_depth;     /* Number of valid stack frames */
};

static struct {
    struct perf_pf_sample samples[PERF_PF_BUF_SIZE];
    volatile uint32_t write_idx;
    int enabled;
    int initialized;
    int sample_rate;          /* 1/N sampling rate (1 = every fault) */
} perf_pf_state;

void perf_pf_init(void)
{
    if (perf_pf_state.initialized) return;
    memset(&perf_pf_state, 0, sizeof(perf_pf_state));
    perf_pf_state.enabled = 1;
    perf_pf_state.sample_rate = 1; /* sample every fault */
    perf_pf_state.initialized = 1;
}

void perf_pf_enable(void)  { perf_pf_state.enabled = 1; }
void perf_pf_disable(void) { perf_pf_state.enabled = 0; }

void perf_pf_set_sample_rate(int rate)
{
    if (rate < 1) rate = 1;
    if (rate > 1000) rate = 1000;
    perf_pf_state.sample_rate = rate;
}

/*
 * Capture a partial stack trace by walking RBP-linked frames.
 * Returns the number of frames captured.
 */
static int perf_capture_stack(uint64_t *frames, int max_frames)
{
    int count = 0;
    uint64_t rbp;

    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));

    while (rbp && count < max_frames) {
        /* Read the return address (rbp + 8) */
        uint64_t ret_addr;
        if (rbp < 0xFFFFFF8000000000ULL || rbp >= 0xFFFFFFFFFFFFFFFFULL)
            break; /* Sanity check */
        if (__builtin_memcpy(&ret_addr, (void*)(rbp + 8), sizeof(ret_addr)))
            break;
        frames[count++] = ret_addr;

        /* Follow frame pointer to the previous frame */
        uint64_t next_rbp;
        if (__builtin_memcpy(&next_rbp, (void*)rbp, sizeof(next_rbp)))
            break;
        rbp = next_rbp;
    }

    return count;
}

void perf_pf_sample(uint64_t fault_addr, uint64_t ip, uint32_t error_code,
                    uint32_t pid)
{
    if (!perf_pf_state.initialized || !perf_pf_state.enabled)
        return;

    /* Apply sampling rate */
    static uint32_t sample_counter = 0;
    sample_counter++;
    if (sample_counter % perf_pf_state.sample_rate != 0)
        return;

    uint32_t idx = __sync_fetch_and_add(&perf_pf_state.write_idx, 1)
                   % PERF_PF_BUF_SIZE;

    struct perf_pf_sample *s = &perf_pf_state.samples[idx];
    memset(s, 0, sizeof(*s));

    s->timestamp = timer_get_ticks();
    s->fault_addr = fault_addr;
    s->ip = ip;
    s->pid = pid;
    s->error_code = error_code;
    s->stack_depth = perf_capture_stack(s->stack, PERF_PF_STACK_DEPTH);
}

int perf_pf_read_samples(struct perf_pf_sample *buf, int max_count)
{
    if (!buf || max_count <= 0)
        return 0;

    uint32_t count = perf_pf_state.write_idx;
    if (count > PERF_PF_BUF_SIZE)
        count = PERF_PF_BUF_SIZE;
    if ((int)count > max_count)
        count = (uint32_t)max_count;

    for (uint32_t i = 0; i < count; i++) {
        buf[i] = perf_pf_state.samples[i];
    }
    return (int)count;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Perf Flame Graph Support — Folded Stack Output (Item 6)
 * ══════════════════════════════════════════════════════════════════════════
 *
 * Produces "folded stack" format consumed by Brendan Gregg's flamegraph.pl.
 *
 * Format:
 *   func_a;func_b;func_c <count>
 *
 * Each line is a semicolon-separated call chain followed by a space and
 * the sample count.  Multiple identical stacks are aggregated.
 *
 * Uses the kernel's symbol resolution (ksym/kallsyms) to translate
 * addresses to function names.
 */

/* Maximum unique stack traces we can track */
#define FLAME_MAX_STACKS 2048
#define FLAME_MAX_FRAMES 32
#define FLAME_SYM_LEN    64

struct flame_stack {
    uint64_t frames[FLAME_MAX_FRAMES];
    int      depth;
    uint64_t count;
};

static struct {
    struct flame_stack stacks[FLAME_MAX_STACKS];
    int num_stacks;
    int enabled;
    int initialized;
} flame_state;

void perf_flame_init(void)
{
    if (flame_state.initialized) return;
    memset(&flame_state, 0, sizeof(flame_state));
    flame_state.enabled = 1;
    flame_state.initialized = 1;
}

void perf_flame_enable(void)  { flame_state.enabled = 1; }
void perf_flame_disable(void) { flame_state.enabled = 0; }

/* Compare two stack frames for equality */
static int flame_stack_equal(const uint64_t *a, int depth_a,
                              const uint64_t *b, int depth_b)
{
    if (depth_a != depth_b) return 0;
    for (int i = 0; i < depth_a; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

/* Add a sample (stack trace) to the flame graph aggregator. */
void perf_flame_add_sample(const uint64_t *frames, int depth)
{
    if (!flame_state.initialized || !flame_state.enabled || !frames)
        return;

    if (depth <= 0) return;
    if (depth > FLAME_MAX_FRAMES) depth = FLAME_MAX_FRAMES;

    /* Look for an existing matching stack */
    for (int i = 0; i < flame_state.num_stacks; i++) {
        if (flame_stack_equal(flame_state.stacks[i].frames,
                               flame_state.stacks[i].depth,
                               frames, depth)) {
            flame_state.stacks[i].count++;
            return;
        }
    }

    /* Add a new stack entry */
    if (flame_state.num_stacks >= FLAME_MAX_STACKS)
        return;

    struct flame_stack *fs = &flame_state.stacks[flame_state.num_stacks++];
    memcpy(fs->frames, frames, depth * sizeof(uint64_t));
    fs->depth = depth;
    fs->count = 1;
}

/* Helper: resolve a kernel address to a symbol name.
 * Returns the symbol name or "0x<addr>" if unresolved. */
static const char *flame_resolve_symbol(uint64_t addr)
{
    /* Use kallsyms / ksym lookup if available */
    static char sym_buf[FLAME_SYM_LEN];

    const char *name = kallsyms_lookup(addr);
    if (name)
        return name;

    /* Fallback: format as hex */
    snprintf(sym_buf, sizeof(sym_buf), "0x%llx", (unsigned long long)addr);
    return sym_buf;
}

/* Write the folded stack output into a caller-provided buffer.
 * Uses the format: func_a;func_b;func_c <count>\n
 * Returns the number of bytes written. */
int perf_flame_generate(char *buf, int buf_size)
{
    if (!buf || buf_size <= 0)
        return 0;

    int total = 0;

    for (int i = 0; i < flame_state.num_stacks; i++) {
        struct flame_stack *fs = &flame_state.stacks[i];

        /* Build the semicolon-separated call chain */
        int pos = 0;
        for (int f = 0; f < fs->depth; f++) {
            const char *sym = flame_resolve_symbol(fs->frames[f]);
            int remaining = buf_size - total - pos - 1;
            if (remaining <= 0) break;

            if (f > 0) {
                buf[total + pos++] = ';';
            }

            /* Copy symbol name */
            while (*sym && pos < buf_size - total - 1) {
                buf[total + pos++] = *sym++;
            }
        }

        if (pos >= buf_size - total) break;

        /* Append count */
        int n = snprintf(buf + total + pos, buf_size - total - pos,
                        " %llu\n", (unsigned long long)fs->count);
        if (n > 0) pos += n;

        total += pos;
        if (total >= buf_size - 1) break;
    }

    if (total < buf_size)
        buf[total] = '\0';
    else
        buf[buf_size - 1] = '\0';

    return total;
}

/* Clear the flame graph aggregator. */
void perf_flame_clear(void)
{
    memset(flame_state.stacks, 0, sizeof(flame_state.stacks));
    flame_state.num_stacks = 0;
}

/* Return the number of unique stacks tracked. */
int perf_flame_num_stacks(void)
{
    return flame_state.num_stacks;
}

/* ══════════════════════════════════════════════════════════════════════
 * Perf Event Sampling — Context Switch, Page Fault, MMAP tracking
 * ══════════════════════════════════════════════════════════════════════
 *
 * Ring-buffer-based sampling of OS events.  Each event type has its own
 * 4096-entry static ring buffer.  Events are timestamped with nanosecond
 * resolution and readable via /sys/kernel/debug/perf/<type>.
 */

/* ── Context Switch Sampling ───────────────────────────────────────── */

static struct {
    struct perf_cswitch_sample events[PERF_SAMPLE_BUF_SIZE];
    volatile uint32_t write_idx;
    int enabled;
    int initialized;
} g_perf_cswitch;

static spinlock_t g_perf_cswitch_lock;

void perf_cswitch_enable_sampling(void)
{
    g_perf_cswitch.enabled = 1;
}

void perf_cswitch_disable_sampling(void)
{
    g_perf_cswitch.enabled = 0;
}

void perf_context_switch_event(uint32_t prev_pid, uint32_t next_pid,
                                uint32_t reason)
{
    if (!g_perf_cswitch.initialized || !g_perf_cswitch.enabled)
        return;

    uint32_t idx = __sync_fetch_and_add(&g_perf_cswitch.write_idx, 1)
                   % PERF_SAMPLE_BUF_SIZE;

    struct perf_cswitch_sample *s = &g_perf_cswitch.events[idx];
    s->timestamp_ns = timer_get_ns();
    s->prev_pid = prev_pid;
    s->next_pid = next_pid;
    s->reason = reason;
}

int perf_read_cswitch_samples(struct perf_cswitch_sample *buf, int max_count)
{
    if (!buf || max_count <= 0 || !g_perf_cswitch.initialized)
        return 0;

    uint32_t count = g_perf_cswitch.write_idx;
    uint32_t n = (count < PERF_SAMPLE_BUF_SIZE) ? count : PERF_SAMPLE_BUF_SIZE;
    if ((int)n > max_count)
        n = (uint32_t)max_count;

    for (uint32_t i = 0; i < n; i++)
        buf[i] = g_perf_cswitch.events[i];

    return (int)n;
}

void perf_clear_cswitch(void)
{
    if (!g_perf_cswitch.initialized)
        return;
    memset(g_perf_cswitch.events, 0, sizeof(g_perf_cswitch.events));
    g_perf_cswitch.write_idx = 0;
}

/* ── Page Fault Sampling ───────────────────────────────────────────── */

static struct {
    struct perf_pf_sample_v2 events[PERF_SAMPLE_BUF_SIZE];
    volatile uint32_t write_idx;
    int enabled;
    int initialized;
} g_perf_pf_v2;

static spinlock_t g_perf_pf_v2_lock;

void perf_pf_enable_sampling(void)
{
    g_perf_pf_v2.enabled = 1;
}

void perf_pf_disable_sampling(void)
{
    g_perf_pf_v2.enabled = 0;
}

void perf_page_fault_event(uint64_t addr, uint32_t flags, uint32_t pid)
{
    if (!g_perf_pf_v2.initialized || !g_perf_pf_v2.enabled)
        return;

    uint32_t idx = __sync_fetch_and_add(&g_perf_pf_v2.write_idx, 1)
                   % PERF_SAMPLE_BUF_SIZE;

    struct perf_pf_sample_v2 *s = &g_perf_pf_v2.events[idx];
    s->timestamp_ns = timer_get_ns();
    s->addr = addr;
    s->flags = flags;
    s->pid = pid;
}

int perf_read_pf_samples(struct perf_pf_sample_v2 *buf, int max_count)
{
    if (!buf || max_count <= 0 || !g_perf_pf_v2.initialized)
        return 0;

    uint32_t count = g_perf_pf_v2.write_idx;
    uint32_t n = (count < PERF_SAMPLE_BUF_SIZE) ? count : PERF_SAMPLE_BUF_SIZE;
    if ((int)n > max_count)
        n = (uint32_t)max_count;

    for (uint32_t i = 0; i < n; i++)
        buf[i] = g_perf_pf_v2.events[i];

    return (int)n;
}

void perf_clear_pf(void)
{
    if (!g_perf_pf_v2.initialized)
        return;
    memset(g_perf_pf_v2.events, 0, sizeof(g_perf_pf_v2.events));
    g_perf_pf_v2.write_idx = 0;
}

/* ── MMAP/Munmap Tracking ──────────────────────────────────────────── */

static struct {
    struct perf_mmap_sample events[PERF_SAMPLE_BUF_SIZE];
    volatile uint32_t write_idx;
    int enabled;
    int initialized;
} g_perf_mmap;

static spinlock_t g_perf_mmap_lock;

void perf_mmap_enable_sampling(void)
{
    g_perf_mmap.enabled = 1;
}

void perf_mmap_disable_sampling(void)
{
    g_perf_mmap.enabled = 0;
}

void perf_mmap_event(uint32_t pid, uint64_t addr, uint64_t len,
                      uint32_t flags)
{
    if (!g_perf_mmap.initialized || !g_perf_mmap.enabled)
        return;

    uint32_t idx = __sync_fetch_and_add(&g_perf_mmap.write_idx, 1)
                   % PERF_SAMPLE_BUF_SIZE;

    struct perf_mmap_sample *s = &g_perf_mmap.events[idx];
    s->timestamp_ns = timer_get_ns();
    s->addr = addr;
    s->len = len;
    s->pid = pid;
    s->flags = flags;
}

int perf_read_mmap_samples(struct perf_mmap_sample *buf, int max_count)
{
    if (!buf || max_count <= 0 || !g_perf_mmap.initialized)
        return 0;

    uint32_t count = g_perf_mmap.write_idx;
    uint32_t n = (count < PERF_SAMPLE_BUF_SIZE) ? count : PERF_SAMPLE_BUF_SIZE;
    if ((int)n > max_count)
        n = (uint32_t)max_count;

    for (uint32_t i = 0; i < n; i++)
        buf[i] = g_perf_mmap.events[i];

    return (int)n;
}

void perf_clear_mmap(void)
{
    if (!g_perf_mmap.initialized)
        return;
    memset(g_perf_mmap.events, 0, sizeof(g_perf_mmap.events));
    g_perf_mmap.write_idx = 0;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void perf_sample_init(void)
{
    /* Context switch buffer */
    if (!g_perf_cswitch.initialized) {
        memset(&g_perf_cswitch, 0, sizeof(g_perf_cswitch));
        g_perf_cswitch.initialized = 1;
        g_perf_cswitch.enabled = 1;
        spinlock_init(&g_perf_cswitch_lock);
    }

    /* Page fault buffer */
    if (!g_perf_pf_v2.initialized) {
        memset(&g_perf_pf_v2, 0, sizeof(g_perf_pf_v2));
        g_perf_pf_v2.initialized = 1;
        g_perf_pf_v2.enabled = 1;
        spinlock_init(&g_perf_pf_v2_lock);
    }

    /* MMAP buffer */
    if (!g_perf_mmap.initialized) {
        memset(&g_perf_mmap, 0, sizeof(g_perf_mmap));
        g_perf_mmap.initialized = 1;
        g_perf_mmap.enabled = 1;
        spinlock_init(&g_perf_mmap_lock);
    }

    kprintf("[perf] Sample ring buffers initialized (%d entries each)\n",
            PERF_SAMPLE_BUF_SIZE);
}
