#ifndef PERF_EVENTS_H
#define PERF_EVENTS_H

#include "types.h"

/* MSR definitions for performance counters */
#define IA32_PERF_GLOBAL_CTRL  0x38F
#define IA32_PERFEVTSEL0       0x186
#define IA32_PMC0              0xC1
#define IA32_PMC1              0xC2

/* Fixed-function counters (architectural) */
#define IA32_FIXED_CTR0        0x309
#define IA32_FIXED_CTR0_CTRL   0x38D

/* Software event counters */
struct perf_sw_counters {
    uint64_t context_switches;
    uint64_t page_faults;
    uint64_t cpu_cycles;
    uint64_t instructions;
};

/* Read a hardware performance counter (PMC0-PMC3) */
uint64_t perf_read_pmc(int counter);

/* Enable/disable performance monitoring globally */
void perf_enable(void);
void perf_disable(void);

/* Software event accounting */
void perf_sw_context_switch(void);
void perf_sw_page_fault(void);

/* Read software event counters */
uint64_t perf_sw_read_context_switches(void);
uint64_t perf_sw_read_page_faults(void);

/* Initialize performance monitoring */
void perf_init(void);

#endif /* PERF_EVENTS_H */
