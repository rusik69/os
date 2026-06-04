#ifndef CPUPSTATE_H
#define CPUPSTATE_H

#include "types.h"

/* ACPI P-state control MSRs */
#define MSR_IA32_PLATFORM_ID    0x017
#define MSR_IA32_PERF_STATUS    0x019
#define MSR_IA32_PERF_CTL       0x019  /* Same MSR address on older CPUs */
#define MSR_IA32_PERFEVTSEL0    0x186
#define MSR_IA32_PERF_CTL_ACTUAL 0x199  /* Performance Control MSR */
#define MSR_IA32_PERF_STATUS_ACTUAL 0x198  /* Performance Status MSR */

/* PERF_CTL bits */
#define PERF_CTL_FREQ_MASK     0xFFFF
#define PERF_CTL_VID_SHIFT     8
#define PERF_CTL_VID_MASK      0xFF00
#define PERF_CTL_ID_SHIFT      0
#define PERF_CTL_ID_MASK       0xFF

/* Max P-states */
#define CPUPSTATE_MAX_STATES   16

/* P-state descriptor */
struct cpupstate_state {
    uint32_t core_freq;    /* MHz */
    uint32_t power;        /* mW */
    uint32_t transition_latency; /* us */
    uint8_t  control;      /* Control value (written to MSR) */
    uint8_t  status;       /* Status value */
};

/* CPU P-state controller */
struct cpupstate_ctrl {
    int      present;
    int      num_states;
    int      current_state;
    struct cpupstate_state states[CPUPSTATE_MAX_STATES];
};

/* API */
int  cpupstate_init(void);
int  cpupstate_set_state(int state);
int  cpupstate_get_state(void);
int  cpupstate_get_count(void);
int  cpupstate_get_info(int state, struct cpupstate_state *info);
int  cpupstate_is_present(void);

/* Register P-states discovered by ACPI _PSS (called by acpi_cpufreq).
 * Overrides MSR-probed defaults. Returns 0 on success. */
int  cpufreq_register_acpi_states(const struct cpupstate_state *states, int count);

#endif /* CPUPSTATE_H */
