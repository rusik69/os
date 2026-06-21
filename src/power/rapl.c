/*
 * rapl.c — Running Average Power Limit (RAPL) support
 *
 * Reads RAPL MSRs to expose energy/power information for the PKG, DRAM,
 * and PP0 (core) power domains.  Provides kernel log output and a simple
 * in-kernel API for reading energy counters.
 *
 * MSR references (Intel SDM Vol. 4):
 *   MSR_RAPL_POWER_UNIT     (0x606) — Unit information
 *   MSR_PKG_ENERGY_STATUS   (0x611) — PKG domain energy counter
 *   MSR_DRAM_ENERGY_STATUS  (0x619) — DRAM domain energy counter
 *   MSR_PP0_ENERGY_STATUS   (0x639) — PP0 (core) domain energy counter
 */

#include "rapl.h"
#include "printf.h"
#include "cpu.h"
#include "string.h"

/* ── State ──────────────────────────────────────────────────────────── */

/* Cached unit information */
static int g_rapl_initialized = 0;
static struct rapl_units g_rapl_units;
static int g_rapl_has_pkg = 0;
static int g_rapl_has_dram = 0;
static int g_rapl_has_pp0 = 0;

/* ── Internal helpers ──────────────────────────────────────────────── */

static int rapl_probe(void)
{
    /* Try reading the RAPL power unit MSR.  If it raises a GP fault,
     * RAPL is not supported.  We use a simple check: read MSR and see
     * if we get a reasonable value. */
    uint64_t val = read_msr(MSR_RAPL_POWER_UNIT);

    /* If the MSR reads as all-ones, it's likely not supported */
    if (val == 0 || val == UINT64_MAX) {
        return -1;
    }

    /* Parse unit information */
    uint32_t esu = (uint32_t)(val & 0x1F);          /* Energy Status Unit (bits 0-4) */
    uint32_t psu = (uint32_t)((val >> 8) & 0x1F);   /* Power Unit (bits 8-12) */
    uint32_t tsu = (uint32_t)((val >> 16) & 0x1F);  /* Time Unit (bits 16-20) */

    g_rapl_units.energy = 1.0 / (double)(1ULL << esu);
    g_rapl_units.power  = 1.0 / (double)(1ULL << psu);
    g_rapl_units.time   = 1.0 / (double)(1ULL << tsu);

    /* Check for domain availability by reading status MSRs */
    g_rapl_has_pkg  = (read_msr(MSR_PKG_ENERGY_STATUS) != UINT64_MAX) ? 1 : 0;
    g_rapl_has_dram = (read_msr(MSR_DRAM_ENERGY_STATUS) != UINT64_MAX) ? 1 : 0;
    g_rapl_has_pp0  = (read_msr(MSR_PP0_ENERGY_STATUS) != UINT64_MAX) ? 1 : 0;

    g_rapl_initialized = 1;

    kprintf("[RAPL] Initialized: energy_unit=%.6fJ power_unit=%.6fW time_unit=%.6fs\n",
            g_rapl_units.energy, g_rapl_units.power, g_rapl_units.time);
    kprintf("[RAPL] Domains: PKG=%d DRAM=%d PP0=%d\n",
            g_rapl_has_pkg, g_rapl_has_dram, g_rapl_has_pp0);

    return 0;
}

static int rapl_read_energy(uint32_t msr, struct rapl_domain_energy *out)
{
    if (!g_rapl_initialized)
        return -1;
    if (!out)
        return -1;

    out->raw_value = read_msr(msr);
    out->energy_joules = (double)out->raw_value * g_rapl_units.energy;
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int rapl_init(void)
{
    if (g_rapl_initialized)
        return 0;

    return rapl_probe();
}

int rapl_read_energy_pkg(struct rapl_domain_energy *out)
{
    if (!g_rapl_has_pkg)
        return -1;
    return rapl_read_energy(MSR_PKG_ENERGY_STATUS, out);
}

int rapl_read_energy_dram(struct rapl_domain_energy *out)
{
    if (!g_rapl_has_dram)
        return -1;
    return rapl_read_energy(MSR_DRAM_ENERGY_STATUS, out);
}

int rapl_read_energy_pp0(struct rapl_domain_energy *out)
{
    if (!g_rapl_has_pp0)
        return -1;
    return rapl_read_energy(MSR_PP0_ENERGY_STATUS, out);
}

int rapl_get_units(struct rapl_units *units)
{
    if (!g_rapl_initialized)
        return -1;
    if (!units)
        return -1;

    *units = g_rapl_units;
    return 0;
}

/* ── rapl_set_limit ─────────────────────────────── */
int rapl_set_limit(int domain, uint64_t limit)
{
    (void)domain;
    (void)limit;
    /* RAPL power limiting: write the limit to the MSR for the given domain.
     * For now, just acknowledge the request. Full MSR write will be added
     * when RAPL MSR infrastructure is complete. */
    if (!g_rapl_initialized) return -1;
    return 0;
}
