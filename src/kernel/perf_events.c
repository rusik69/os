#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "perf_events.h"
#include "cpu.h"
#include "heap.h"
#include "string.h"
#include "smp.h"
#include "errno.h"

/* Software event counters */
static struct perf_sw_counters sw_counters;

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
 * Initialisation
 * ══════════════════════════════════════════════════════════════════════════ */

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

    if (edx & (1 << 21)) {
        pebs_platform_ok = 1;
        kprintf("[OK] perf_events: PEBS / Debug Store supported\n");
    } else {
        pebs_platform_ok = 0;
        kprintf("[OK] perf_events: PEBS not available (no DS in CPUID)\n");
    }
}
