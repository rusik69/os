#ifndef RAPL_H
#define RAPL_H

#include "types.h"

/* ── RAPL MSR addresses (Intel) ────────────────────────────────────── */

#define MSR_RAPL_POWER_UNIT     0x606   /* Power/energy/time unit info */
#define MSR_PKG_POWER_LIMIT     0x610   /* PKG power limit */
#define MSR_PKG_ENERGY_STATUS   0x611   /* PKG energy counter */
#define MSR_PKG_POWER_INFO      0x614   /* PKG power info */
#define MSR_DRAM_ENERGY_STATUS  0x619   /* DRAM energy counter */
#define MSR_PP0_ENERGY_STATUS   0x639   /* PP0 (core) energy counter */
#define MSR_PP1_ENERGY_STATUS   0x641   /* PP1 (uncore/graphics) energy counter */
#define MSR_PLATFORM_ENERGY_STATUS 0x64D  /* Platform energy counter */

/* ── Unit information ──────────────────────────────────────────────── */

struct rapl_units {
    double energy;      /* joules per unit (e.g., 1/2^ESU) */
    double power;       /* watts per unit */
    double time;        /* seconds per unit */
};

/* ── Per-domain energy status ──────────────────────────────────────── */

struct rapl_domain_energy {
    uint64_t raw_value;         /* Raw energy counter value */
    double   energy_joules;     /* Energy in joules */
};

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * rapl_init — Read RAPL power unit MSR and log capabilities.
 *
 * Returns 0 on success, -1 if RAPL is not available.
 */
int rapl_init(void);

/**
 * rapl_read_energy_pkg — Read the PKG domain energy counter.
 * @out: Pointer to a rapl_domain_energy struct to fill.
 *
 * Returns 0 on success, -1 if not available.
 */
int rapl_read_energy_pkg(struct rapl_domain_energy *out);

/**
 * rapl_read_energy_dram — Read the DRAM domain energy counter.
 * @out: Pointer to a rapl_domain_energy struct to fill.
 *
 * Returns 0 on success, -1 if not available.
 */
int rapl_read_energy_dram(struct rapl_domain_energy *out);

/**
 * rapl_read_energy_pp0 — Read the PP0 (core) domain energy counter.
 * @out: Pointer to a rapl_domain_energy struct to fill.
 *
 * Returns 0 on success, -1 if not available.
 */
int rapl_read_energy_pp0(struct rapl_domain_energy *out);

/**
 * rapl_get_units — Get the RAPL unit conversion factors.
 * @units: Pointer to a rapl_units struct to fill.
 *
 * Returns 0 on success, -1 if RAPL is not initialized.
 */
int rapl_get_units(struct rapl_units *units);

#endif /* RAPL_H */
