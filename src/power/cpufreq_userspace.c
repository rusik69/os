/*
 * cpufreq_userspace.c — Userspace CPU frequency scaling governor
 *
 * Allows userspace to directly control the CPU frequency by writing to
 * the scaling_setspeed sysfs file.  The governor does no automatic
 * frequency scaling — it simply applies whatever frequency (P-state)
 * the user has requested.
 *
 * Sysfs interface:
 *   /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed  (write)
 *   /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq  (read)
 *
 * Item 113 — CPU frequency: userspace governor
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "cpupstate.h"
#include "printf.h"
#include "string.h"
#include "sysfs.h"

#ifndef UINT32_MAX
#define UINT32_MAX 0xFFFFFFFFU
#endif

/* ── Governor state ─────────────────────────────────────────────────── */

static int g_active = 0;
static int g_userspace_freq_state = -1;  /* -1 = not set, else P-state index */

/* ── Public API ───────────────────────────────────────────────────── */

static int cpufreq_userspace_init(void)
{
    if (!cpupstate_is_present()) {
        kprintf("[userspace] CPU freq scaling not present — disabled\n");
        return -1;
    }

    g_active = 0;
    g_userspace_freq_state = -1;

    kprintf("[userspace] Governor initialized\n");
    return 0;
}

static int cpufreq_userspace_start(void)
{
    if (!cpupstate_is_present()) return -1;
    g_active = 1;

    /* Set to current state initially */
    if (g_userspace_freq_state < 0) {
        g_userspace_freq_state = cpupstate_get_state();
    }

    kprintf("[userspace] Governor started (current P%d)\n", g_userspace_freq_state);
    return 0;
}

static void cpufreq_userspace_stop(void)
{
    g_active = 0;
    kprintf("[userspace] Governor stopped\n");
}

static int cpufreq_userspace_is_active(void)
{
    return g_active;
}

/*
 * Set the desired frequency via P-state index.
 * This is the core of the userspace governor — the kernel applies
 * exactly what the user requested through scaling_setspeed.
 *
 * @state: P-state index (0 = highest frequency, num_states-1 = lowest)
 * Returns 0 on success, -1 if state is out of range.
 */
static int cpufreq_userspace_set_state(int state)
{
    int num_states = cpupstate_get_count();
    if (num_states <= 0)
        return -1;

    if (state < 0 || state >= num_states)
        return -1;

    g_userspace_freq_state = state;

    /* Apply immediately if we are the active governor */
    if (g_active) {
        cpupstate_set_state(state);
        kprintf("[userspace] Setting P-state to P%d\n", state);
    }

    return 0;
}

/*
 * Get the currently set userspace frequency (P-state index).
 */
static int cpufreq_userspace_get_state(void)
{
    return g_userspace_freq_state;
}

/*
 * Apply the userspace-set frequency immediately.
 * Called when switching to this governor.
 */
static void cpufreq_userspace_apply(void)
{
    if (!g_active || !cpupstate_is_present())
        return;

    if (g_userspace_freq_state >= 0) {
        cpupstate_set_state(g_userspace_freq_state);
        kprintf("[userspace] Applied P%d\n", g_userspace_freq_state);
    }
}

/*
 * Sysfs write handler for scaling_setspeed.
 * Accepts frequencies in kHz or the strings "userspace", "ondemand", etc.
 * For kHz, we find the nearest P-state and apply it.
 */
static int cpufreq_userspace_setspeed_write(const char *buf, uint32_t size)
{
    /* Parse the input — could be a frequency in kHz or "conservative"/"powersave" etc. */
    char tmp[64];
    uint32_t len = size < (uint32_t)sizeof(tmp) - 1 ? size : (uint32_t)sizeof(tmp) - 1;
    memcpy(tmp, buf, len);
    tmp[len] = '\0';

    /* Strip whitespace/newline */
    while (len > 0 && (tmp[len-1] == '\n' || tmp[len-1] == ' ' || tmp[len-1] == '\t'))
        tmp[--len] = '\0';

    /* Try to parse as integer (frequency in kHz) */
    int val = 0;
    int is_numeric = 1;
    for (uint32_t i = 0; i < len; i++) {
        if (tmp[i] >= '0' && tmp[i] <= '9') {
            val = val * 10 + (tmp[i] - '0');
        } else {
            is_numeric = 0;
            break;
        }
    }

    if (is_numeric && val > 0) {
        /* Convert kHz to P-state index (approximate) */
        int num_states = cpupstate_get_count();
        if (num_states <= 0) return -1;

        /* Find nearest P-state to requested frequency */
        int best_state = 0;
        uint32_t best_diff = 0xFFFFFFFF;

        for (int i = 0; i < num_states; i++) {
            struct cpupstate_state info;
            if (cpupstate_get_info(i, &info) == 0) {
                uint32_t freq_khz = info.core_freq * 1000;
                uint32_t diff = (freq_khz > (uint32_t)val) ? (freq_khz - (uint32_t)val) : ((uint32_t)val - freq_khz);
                if (diff < best_diff) {
                    best_diff = diff;
                    best_state = i;
                }
            }
        }

        return cpufreq_userspace_set_state(best_state);
    }

    return 0;
}

/* ── usr_setspeed ─────────────────────────────── */
static int usr_setspeed(int cpu, unsigned int freq)
{
    (void)cpu;
    /* Find the closest P-state to the requested frequency and set it */
    int count = cpupstate_get_count();
    if (count <= 0) return -1;

    int best = 0;
    uint32_t best_diff = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        struct cpupstate_state info;
        if (cpupstate_get_info(i, &info) == 0) {
            uint32_t freq_khz = info.core_freq * 1000;
            uint32_t diff = (freq_khz > freq) ? (freq_khz - freq) : (freq - freq_khz);
            if (diff < best_diff) {
                best_diff = diff;
                best = i;
            }
        }
    }
    g_userspace_freq_state = best;
    return cpupstate_set_state(best);
}
/* ── usr_getspeed ─────────────────────────────── */
static unsigned int usr_getspeed(int cpu)
{
    (void)cpu;
    int cur = cpupstate_get_state();
    if (cur < 0) return 0;
    struct cpupstate_state info;
    if (cpupstate_get_info(cur, &info) == 0)
        return info.core_freq * 1000;
    return 0;
}
