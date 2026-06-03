#include "timeconst.h"
#include "printf.h"
#include "kernel.h"

/*
 * Time constant subsystem.
 *
 * Pre-computes and caches timing conversion factors for the
 * configured system tick rate.
 */

/* System tick rate (configurable, default 1000 Hz) */
static uint32_t system_hz = TIMECONST_HZ_1000;

/* Cached conversion factors */
static uint64_t cached_ns_per_tick;
static uint64_t cached_mult;
static __attribute__((unused)) unsigned int cached_shift = TIMECONST_SHIFT;

void timeconst_init(void)
{
    cached_ns_per_tick = timeconst_ns_per_tick(system_hz);
    cached_mult = timeconst_mult(system_hz);

    kprintf("[OK] timeconst: Time constants initialised (hz=%u, ns/tick=%llu)\n",
            system_hz, cached_ns_per_tick);
}
