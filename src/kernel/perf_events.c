#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "perf_events.h"
#include "cpu.h"

/* Software event counters */
static struct perf_sw_counters sw_counters;

/* Check if the CPU supports architectural PMU */
static int perf_available = 0;

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

void perf_init(void)
{
    /* Check for PMU availability via CPUID */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x0a), "c"(0));
    /* If eax[0:7] > 0, architectural PMU is present */
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

        kprintf("[OK] perf_events: %d PMC counters available\n", (int)(eax >> 8) & 0xFF);
    } else {
        perf_available = 0;
        kprintf("[OK] perf_events: no hardware PMU, software counters only\n");
    }
}
