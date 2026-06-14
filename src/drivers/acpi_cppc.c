// SPDX-License-Identifier: GPL-2.0-only
/*
 * acpi_cppc.c — ACPI CPPC (Collaborative Processor Performance Control)
 *
 * Implements ACPI CPPC interface for collaborative processor
 * performance control between OS and firmware.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "acpi.h"

#define CPPC_MAX_DOMAINS 8

struct cppc_domain {
    int active;
    uint32_t domain_id;
    uint32_t highest_perf;
    uint32_t nominal_perf;
    uint32_t lowest_nonlinear_perf;
    uint32_t lowest_perf;
    uint32_t desired_perf;
    uint64_t reference_perf_counter;
    uint64_t reference_ctr_counter;
    uint64_t delivered_perf_counter;
};

static struct cppc_domain cppc_domains[CPPC_MAX_DOMAINS];
static int cppc_domain_count;

/* Register a CPPC domain */
int cppc_register_domain(uint32_t domain_id,
                          uint32_t highest, uint32_t nominal,
                          uint32_t lowest_nonlinear, uint32_t lowest)
{
    if (cppc_domain_count >= CPPC_MAX_DOMAINS)
        return -ENOMEM;

    struct cppc_domain *dom = &cppc_domains[cppc_domain_count];
    dom->active = 1;
    dom->domain_id = domain_id;
    dom->highest_perf = highest;
    dom->nominal_perf = nominal;
    dom->lowest_nonlinear_perf = lowest_nonlinear;
    dom->lowest_perf = lowest;
    dom->desired_perf = nominal;
    cppc_domain_count++;

    kprintf("[CPPC] Domain %u registered: highest=%u nominal=%u lowest=%u\n",
            domain_id, highest, nominal, lowest);
    return cppc_domain_count - 1;
}

/* Set desired performance */
int cppc_set_desired_perf(int dom_idx, uint32_t perf)
{
    if (dom_idx < 0 || dom_idx >= cppc_domain_count || !cppc_domains[dom_idx].active)
        return -ENODEV;

    struct cppc_domain *dom = &cppc_domains[dom_idx];
    if (perf < dom->lowest_perf) perf = dom->lowest_perf;
    if (perf > dom->highest_perf) perf = dom->highest_perf;
    dom->desired_perf = perf;

    return 0;
}

/* Get current performance */
uint32_t cppc_get_desired_perf(int dom_idx)
{
    if (dom_idx < 0 || dom_idx >= cppc_domain_count || !cppc_domains[dom_idx].active)
        return 0;

    return cppc_domains[dom_idx].desired_perf;
}

/* Get domain info */
void cppc_get_info(int dom_idx, uint32_t *highest, uint32_t *nominal,
                    uint32_t *lowest_nonlinear, uint32_t *lowest)
{
    if (dom_idx < 0 || dom_idx >= cppc_domain_count || !cppc_domains[dom_idx].active)
        return;

    struct cppc_domain *dom = &cppc_domains[dom_idx];
    if (highest) *highest = dom->highest_perf;
    if (nominal) *nominal = dom->nominal_perf;
    if (lowest_nonlinear) *lowest_nonlinear = dom->lowest_nonlinear_perf;
    if (lowest) *lowest = dom->lowest_perf;
}

void acpi_cppc_init(void)
{
    memset(cppc_domains, 0, sizeof(cppc_domains));
    cppc_domain_count = 0;

    /* Try to read CPPC tables from ACPI */
    /* In real implementation, parse _CPC objects from ACPI */

    kprintf("[OK] ACPI CPPC — Collaborative Processor Performance Control\n");
}
