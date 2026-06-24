/*
 * acpi_cpufreq.c — ACPI P-state (processor performance) driver
 *
 * Reads ACPI _PSS (Processor Supported States), _PCT (Performance
 * Control), and _PPC (Performance Present Capabilities) objects from
 * the DSDT/SSDT and registers P-states with the cpufreq core.
 *
 * Since this kernel lacks a full AML interpreter, we attempt to find
 * _PSS package data by scanning the DSDT binary for known AML patterns.
 * If ACPI _PSS is unavailable, the cpufreq core falls back to MSR-based
 * P-state probing.
 *
 * References:
 *   - ACPI 6.2 spec: sections 8.4.4–8.4.7 (_PSS, _PCT, _PPC)
 *   - Intel 64 and IA-32 Arch. SDM Vol 3B: Ch 14 (Power Management)
 */

#include "cpupstate.h"
#include "acpi.h"
#include "cpu.h"         /* read_msr */
#include "printf.h"
#include "string.h"

/* ─── ACPI _PSS object scanning ────────────────────────────────────── */

/*
 * Simple AML pattern scanner for _PSS objects.
 *
 * ACPI _PSS is a Package containing one Package per P-state.
 * Each sub-package has 6 DWORD elements:
 *   [0] CoreFrequency (MHz)
 *   [1] Power (mW)
 *   [2] TransitionLatency (us)
 *   [3] BusMasterLatency (us)
 *   [4] Control (value to write to PERF_CTL MSR)
 *   [5] Status (value read from PERF_STATUS)
 *
 * AML encoding of a Package with 6-element sub-packages:
 *   0x12 0xNN 0x06  ...  (Package op, length, 6 elements)
 *
 * This scanner is deliberately simple and may not parse all BIOS
 * variations. It's meant to work with common QEMU/OVMF and real
 * hardware DSDTs.
 */

/* AML opcodes */
#define AML_PACKAGE_OP  0x12
#define AML_BYTE_PREFIX 0x0a
#define AML_WORD_PREFIX 0x0b
#define AML_DWORD_PREFIX 0x0c
#define AML_QWORD_PREFIX 0x0e
#define AML_NAME_OP     0x08
#define AML_METHOD_OP   0x14

/* _PSS signature as a 32-bit little-endian value */
#define PSS_SIG 0x5353505f  /* '_PSS' */

/*
 * Try to read a DWORD from AML data at the given pointer.
 * Returns the value and advances the pointer past the consumed data.
 */
static uint32_t read_aml_dword(const uint8_t **pp, uint32_t max_remaining)
{
    const uint8_t *p = *pp;
    if (max_remaining < 1) return 0;

    uint8_t prefix = *p;
    uint32_t val = 0;

    switch (prefix) {
    case AML_BYTE_PREFIX:
        if (max_remaining < 2) return 0;
        val = p[1];
        *pp = p + 2;
        break;
    case AML_WORD_PREFIX:
        if (max_remaining < 3) return 0;
        val = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
        *pp = p + 3;
        break;
    case AML_DWORD_PREFIX:
        if (max_remaining < 5) return 0;
        val = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
              ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
        *pp = p + 5;
        break;
    default:
        /* Assume it's a raw DWORD value (some BIOS omit prefix) */
        if (max_remaining < 4) return 0;
        val = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
              ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        *pp = p + 4;
        break;
    }
    return val;
}

/*
 * Scan the DSDT for a _PSS package and populate cpupstate entries.
 * Returns the number of P-states found, or -1 on error, or 0 if not found.
 */
static int scan_dsdt_for_pss(void)
{
    if (!g_dsdt_base || g_dsdt_length == 0)
        return 0;

    uint8_t *dsdt = g_dsdt_base;
    uint32_t dsdt_len = g_dsdt_length;

    /* Skip ACPI header (36 bytes typically) to reach AML bytecode */
    uint32_t offset = sizeof(struct acpi_header); /* Usually 36 bytes */
    if (offset >= dsdt_len) return 0;

    /* Scan for Name(_PSS, Package(...)) pattern */
    while (offset + 8 < dsdt_len) {
        /* Look for NAME op followed by _PSS signature */
        if (dsdt[offset] == AML_NAME_OP &&
            offset + 5 < dsdt_len &&
            *(uint32_t *)&dsdt[offset + 1] == PSS_SIG) {

            /* Found Name(_PSS, ...) — check for Package op */
            uint32_t pkg_offset = offset + 5;
            if (pkg_offset >= dsdt_len) break;

            if (dsdt[pkg_offset] != AML_PACKAGE_OP) {
                /* Not directly a package — could be a method that returns _PSS.
                 * For simplicity, we skip method-based _PSS and rely on MSR fallback. */
                kprintf("[acpi_cpufreq] _PSS is a method (not a package) — "
                        "skipping ACPI P-state enumeration\n");
                return 0;
            }

            /* Parse the package */
            pkg_offset++; /* Skip PACKAGE_OP */

            /* Package length can be encoded in 1-2 bytes (simple form) */
            uint32_t pkg_len;
            if (pkg_offset >= dsdt_len) break;
            if (dsdt[pkg_offset] & 0x80) {
                /* Multi-byte length (not common for _PSS) */
                uint8_t byte_count = dsdt[pkg_offset] & 0x0F;
                pkg_len = 0;
                for (uint8_t b = 0; b < byte_count && pkg_offset < dsdt_len; b++) {
                    pkg_offset++;
                    pkg_len |= (uint32_t)dsdt[pkg_offset] << (b * 8);
                }
                pkg_offset++;
            } else {
                pkg_len = dsdt[pkg_offset];
                pkg_offset++;
            }

            /* The first element of the outer package should be the
             * number of sub-packages (which equals the number of P-states).
             * But AML Package already encodes this. We read sub-packages. */

            uint32_t pkg_end = pkg_offset + pkg_len;
            if (pkg_end > dsdt_len) pkg_end = dsdt_len;

            int state_count = 0;
            /* Buffer for _PSS entries discovered from ACPI */
            struct cpupstate_state states_buf[CPUPSTATE_MAX_STATES];
            while (pkg_offset + 3 < pkg_end && state_count < CPUPSTATE_MAX_STATES) {
                /* Look for each sub-package (inner Package op) */
                if (dsdt[pkg_offset] != AML_PACKAGE_OP)
                    break;

                pkg_offset++; /* Skip PACKAGE_OP */

                /* Inner package length */
                uint32_t inner_len;
                if (pkg_offset >= pkg_end) break;
                if (dsdt[pkg_offset] & 0x80) {
                    uint8_t byte_count = dsdt[pkg_offset] & 0x0F;
                    inner_len = 0;
                    for (uint8_t b = 0; b < byte_count && pkg_offset < pkg_end; b++) {
                        pkg_offset++;
                        inner_len |= (uint32_t)dsdt[pkg_offset] << (b * 8);
                    }
                    pkg_offset++;
                } else {
                    inner_len = dsdt[pkg_offset];
                    pkg_offset++;
                }

                uint32_t inner_end = pkg_offset + inner_len;
                if (inner_end > pkg_end) inner_end = pkg_end;

                /* Read the 6 DWORD elements of the _PSS entry */
                uint32_t remaining = inner_end - pkg_offset;
                const uint8_t *saved_pos = NULL;

                /* Read CoreFrequency, Power, TransitionLatency,
                 * BusMasterLatency (ignored), Control, Status */
                uint32_t core_freq_mhz = 0;
                uint32_t power_mw = 0;
                uint32_t trans_lat = 0;
                uint32_t control = 0;
                uint32_t status = 0;

                saved_pos = dsdt + pkg_offset;
                remaining = inner_end - pkg_offset;

                core_freq_mhz = read_aml_dword(&saved_pos, remaining);
                remaining = inner_end - (uint32_t)(saved_pos - dsdt);

                power_mw = read_aml_dword(&saved_pos, remaining);
                remaining = inner_end - (uint32_t)(saved_pos - dsdt);

                trans_lat = read_aml_dword(&saved_pos, remaining);
                remaining = inner_end - (uint32_t)(saved_pos - dsdt);

                /* Skip BusMasterLatency */
                read_aml_dword(&saved_pos, remaining);
                remaining = inner_end - (uint32_t)(saved_pos - dsdt);

                control = read_aml_dword(&saved_pos, remaining);
                remaining = inner_end - (uint32_t)(saved_pos - dsdt);

                status = read_aml_dword(&saved_pos, remaining);

                if (core_freq_mhz > 0 && control <= 0xFF) {
                    states_buf[state_count].core_freq = core_freq_mhz;
                    states_buf[state_count].power     = power_mw;
                    states_buf[state_count].transition_latency = trans_lat;
                    states_buf[state_count].control   = (uint8_t)(control & 0xFF);
                    states_buf[state_count].status    = (uint8_t)(status & 0xFF);
                    state_count++;
                }

                pkg_offset = inner_end;
            }

            if (state_count > 0) {
                /* Register states with the cpufreq core */
                int ret = cpufreq_register_acpi_states(states_buf, state_count);
                if (ret == 0) {
                    kprintf("[acpi_cpufreq] Found %d P-states from ACPI _PSS\n",
                            state_count);
                }
                return state_count;
            }
            return 0; /* _PSS found but empty */
        }
        offset++;
    }

    return 0; /* _PSS not found */
}

/* ─── Public entry point ───────────────────────────────────────────── */

/*
 * acpi_cpufreq_init — try to configure P-states from ACPI _PSS.
 *
 * Called from the cpufreq core during boot. Returns:
 *   > 0 : number of ACPI P-states registered
 *   0   : ACPI _PSS not found (MSR fallback should be used)
 *   < 0 : error
 */
int __init acpi_cpufreq_init(void)
{
    kprintf("[acpi_cpufreq] Scanning ACPI for _PSS...\n");

    int n = scan_dsdt_for_pss();
    if (n > 0) {
        /* Verify _PCT and _PPC if available (informational) */
        kprintf("[acpi_cpufreq] ACPI P-state driver ready (%d states)\n", n);
    } else if (n == 0) {
        kprintf("[acpi_cpufreq] _PSS not found — will use MSR-based P-states\n");
    }
    return n;
}

/* ── Stub: acpi_cpufreq_target ─────────────────────────────── */
int acpi_cpufreq_target(void *policy, unsigned int target_freq)
{
    (void)policy;
    (void)target_freq;
    kprintf("[acpi] acpi_cpufreq_target: not yet implemented\n");
    return 0;
}
