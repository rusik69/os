/*
 * energy_model.c — Energy Model for Energy-Aware Scheduling (EAS)
 *
 * Provides per-CPU and per-device power cost tables that the scheduler
 * can use for energy-aware task placement.  The energy model consists
 * of a table of (frequency, power) pairs for each CPU or device.
 *
 * Design:
 *   - Each CPU has an energy model table indexed by P-state.
 *   - The scheduler can query the estimated energy consumption of
 *     running a task at a given frequency on a given CPU.
 *   - Dynamic power is computed as: P_dyn = C * V^2 * f
 *   - Static (leakage) power is provided per state.
 *
 * Item 125 — Energy model
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "cpu_topology.h"
#include "cpupstate.h"

/* ── Constants ──────────────────────────────────────────────────────── */

#define EM_MAX_CPUS           64
#define EM_MAX_STATES         16
#define EM_MAX_DEVICES        8

/* ── Energy model per state ────────────────────────────────────────── */

struct em_state {
    uint32_t freq_khz;      /* Operating frequency in kHz */
    uint32_t power_mw;      /* Total power (dynamic + static) in mW */
    uint32_t dynamic_mw;    /* Dynamic power in mW */
    uint32_t static_mw;     /* Static (leakage) power in mW */
    uint32_t voltage_mv;    /* Voltage in mV (for CxV²xf computation) */
    uint32_t cost;          /* Normalized energy cost (relative to max) */
};

/* ── Energy model for a CPU ────────────────────────────────────────── */

struct em_cpu {
    int present;
    uint32_t cpu_id;
    int num_states;
    struct em_state states[EM_MAX_STATES];
    uint32_t max_power_mw;   /* Power at highest OPP */
    uint32_t min_power_mw;   /* Power at lowest OPP */
    uint32_t idle_power_mw;  /* Power when idle (static only) */
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct em_cpu em_cpus[EM_MAX_CPUS];
static int em_initialized = 0;

/* ── Internal helpers ──────────────────────────────────────────────── */

/*
 * Estimate dynamic power using the formula:
 *   P_dyn ≈ C * V² * f
 * where C is the capacitance coefficient.
 *
 * For simplicity, we use a linear scaling from the provided maximum
 * power value.
 */
static uint32_t em_compute_dynamic_power(uint32_t freq_khz, uint32_t voltage_mv,
                                          uint32_t max_freq_khz, uint32_t max_voltage_mv,
                                          uint32_t max_dynamic_mw)
{
    if (max_freq_khz == 0 || max_voltage_mv == 0)
        return 0;

    /* Normalize: P = P_max * (f/f_max) * (V/V_max)² */
    uint64_t ratio = (uint64_t)freq_khz * 1000000ULL / max_freq_khz *
                     voltage_mv * voltage_mv / (max_voltage_mv * max_voltage_mv);
    uint64_t power = (uint64_t)max_dynamic_mw * ratio / 1000000ULL;
    return (uint32_t)power;
}

/* ── Public API ─────────────────────────────────────────────────────── */

void em_init(void)
{
    if (em_initialized) return;

    memset(em_cpus, 0, sizeof(em_cpus));
    em_initialized = 1;

    kprintf("[EM] Energy model framework initialized\n");
}

/*
 * Register an energy model table for a CPU.
 * Called by cpufreq or platform code after P-states are discovered.
 *
 * @cpu_id:      Logical CPU number
 * @states:      Array of (freq, power) pairs sorted by freq ascending
 * @num_states:  Number of states in the table
 * @idle_power:  Power consumption when the CPU is idle (static leakage)
 */
int em_register_cpu(uint32_t cpu_id,
                     const struct em_state *states,
                     int num_states,
                     uint32_t idle_power_mw)
{
    if (!em_initialized)
        return -1;

    if (cpu_id >= EM_MAX_CPUS || !states || num_states <= 0 || num_states > EM_MAX_STATES)
        return -1;

    struct em_cpu *cpu = &em_cpus[cpu_id];
    cpu->present = 1;
    cpu->cpu_id = cpu_id;
    cpu->num_states = num_states;
    cpu->idle_power_mw = idle_power_mw;

    uint32_t max_power = 0;
    uint32_t min_power = 0xFFFFFFFF;

    for (int i = 0; i < num_states; i++) {
        cpu->states[i] = states[i];

        if (states[i].power_mw > max_power)
            max_power = states[i].power_mw;
        if (states[i].power_mw < min_power)
            min_power = states[i].power_mw;
    }

    cpu->max_power_mw = max_power;
    cpu->min_power_mw = min_power;

    kprintf("[EM] Registered CPU%u energy model: %d states, %u-%u mW, idle %u mW\n",
            cpu_id, num_states, min_power, max_power, idle_power_mw);
    return 0;
}

/*
 * Build an energy model table from cpupstate data.
 * This auto-generates the table based on the P-states discovered
 * by cpufreq.
 */
int em_register_cpu_from_cpupstate(uint32_t cpu_id, uint32_t max_power_mw,
                                    uint32_t idle_power_mw)
{
    int num_states = cpupstate_get_count();
    if (num_states <= 0 || num_states > EM_MAX_STATES)
        return -1;

    struct em_state states[EM_MAX_STATES];
    memset(states, 0, sizeof(states));

    for (int i = 0; i < num_states; i++) {
        struct cpupstate_state ps;
        if (cpupstate_get_info(i, &ps) < 0)
            continue;

        states[i].freq_khz = ps.core_freq * 1000;
        states[i].voltage_mv = 1200; /* Assume 1.2V at max, scale linearly */

        /* Scale power proportionally to frequency (simplified) */
        if (num_states > 1) {
            uint64_t freq_ratio = (uint64_t)(num_states - 1 - i) * 1000ULL / (num_states - 1);
            states[i].power_mw = (uint32_t)(((uint64_t)max_power_mw * freq_ratio / 1000ULL) + idle_power_mw);
            states[i].dynamic_mw = states[i].power_mw - idle_power_mw;
        } else {
            states[i].power_mw = max_power_mw + idle_power_mw;
            states[i].dynamic_mw = max_power_mw;
        }
        states[i].static_mw = idle_power_mw;
        states[i].cost = (uint32_t)((uint64_t)states[i].power_mw * 1000ULL /
                                     (states[0].power_mw ? states[0].power_mw : 1));
    }

    return em_register_cpu(cpu_id, states, num_states, idle_power_mw);
}

/*
 * Query the estimated energy cost (in mW) of running at a given
 * frequency on a given CPU.
 *
 * @cpu_id:  Logical CPU number
 * @freq_idx: P-state index (0 = highest freq, num_states-1 = lowest)
 * @returns  Estimated power in mW, or 0 if invalid.
 */
uint32_t em_get_power(uint32_t cpu_id, int freq_idx)
{
    if (!em_initialized || cpu_id >= EM_MAX_CPUS)
        return 0;

    struct em_cpu *cpu = &em_cpus[cpu_id];
    if (!cpu->present || freq_idx < 0 || freq_idx >= cpu->num_states)
        return 0;

    return cpu->states[freq_idx].power_mw;
}

/*
 * Get the idle power consumption for a CPU.
 */
uint32_t em_get_idle_power(uint32_t cpu_id)
{
    if (!em_initialized || cpu_id >= EM_MAX_CPUS)
        return 0;

    struct em_cpu *cpu = &em_cpus[cpu_id];
    if (!cpu->present)
        return 0;

    return cpu->idle_power_mw;
}

/*
 * Estimate the total energy for running a task on a given CPU at a given
 * frequency for a given duration.
 *
 * @cpu_id:   Target CPU
 * @freq_idx: P-state index
 * @time_us:  Execution time in microseconds
 * @returns   Estimated energy in nanojoule (nJ)
 */
uint64_t em_estimate_energy(uint32_t cpu_id, int freq_idx, uint64_t time_us)
{
    uint32_t power_mw = em_get_power(cpu_id, freq_idx);
    if (power_mw == 0)
        return 0;

    /* Energy (nJ) = Power (mW) × Time (us) */
    return (uint64_t)power_mw * time_us * 1000ULL;
}

/*
 * Get number of states for a CPU's energy model.
 */
int em_get_num_states(uint32_t cpu_id)
{
    if (!em_initialized || cpu_id >= EM_MAX_CPUS)
        return 0;

    struct em_cpu *cpu = &em_cpus[cpu_id];
    if (!cpu->present)
        return 0;

    return cpu->num_states;
}

/*
 * Get state info for a CPU.
 */
const struct em_state *em_get_state(uint32_t cpu_id, int idx)
{
    if (!em_initialized || cpu_id >= EM_MAX_CPUS)
        return NULL;

    struct em_cpu *cpu = &em_cpus[cpu_id];
    if (!cpu->present || idx < 0 || idx >= cpu->num_states)
        return NULL;

    return &cpu->states[idx];
}

/*
 * Print energy model info for all registered CPUs.
 */
void em_dump(void)
{
    if (!em_initialized) return;

    kprintf("Energy Model:\n");
    for (uint32_t cpu = 0; cpu < EM_MAX_CPUS; cpu++) {
        struct em_cpu *c = &em_cpus[cpu];
        if (!c->present) continue;

        kprintf("  CPU%u: %d states, %u-%u mW, idle %u mW\n",
                cpu, c->num_states, c->min_power_mw, c->max_power_mw,
                c->idle_power_mw);
        for (int i = 0; i < c->num_states; i++) {
            kprintf("    [%d] %u kHz, %u mW (dyn=%u, st=%u, V=%u mV, cost=%u)\n",
                    i,
                    c->states[i].freq_khz,
                    c->states[i].power_mw,
                    c->states[i].dynamic_mw,
                    c->states[i].static_mw,
                    c->states[i].voltage_mv,
                    c->states[i].cost);
        }
    }
}
