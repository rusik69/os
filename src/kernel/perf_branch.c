// SPDX-License-Identifier: GPL-2.0-only
/*
 * perf_branch.c — perf branch stack (LBR) sampling
 *
 * Supports Last Branch Record (LBR) sampling for performance
 * monitoring. Captures branch history for profiling.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "smp.h"

#define PERF_LBR_MAX_ENTRIES 32

struct perf_lbr_entry {
    uint64_t from;
    uint64_t to;
    uint64_t flags; /* branch type, prediction, etc. */
};

struct perf_branch_state {
    struct perf_lbr_entry entries[PERF_LBR_MAX_ENTRIES];
    int count;
    int enabled;
    uint64_t total_branches_sampled;
};

static struct perf_branch_state perf_branch;

/* Enable LBR sampling */
static int perf_branch_enable(void)
{
    perf_branch.enabled = 1;
    perf_branch.count = 0;
    perf_branch.total_branches_sampled = 0;

/* Enable LBR in MSR_IA32_DEBUGCTLMSR */
    uint32_t lo, hi;
    uint64_t debugctl;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1D9));
    debugctl = ((uint64_t)hi << 32) | lo;
    debugctl |= 1; /* LBR = bit 0 */
    lo = (uint32_t)(debugctl & 0xFFFFFFFFULL);
    hi = (uint32_t)(debugctl >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1D9));

    kprintf("[PERF_BRANCH] LBR sampling enabled\n");
    return 0;
}

/* Disable LBR sampling */
static int perf_branch_disable(void)
{
    perf_branch.enabled = 0;

    /* Disable LBR */
    uint32_t lo, hi;
    uint64_t debugctl;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1D9));
    debugctl = ((uint64_t)hi << 32) | lo;
    debugctl &= ~1ULL;
    lo = (uint32_t)(debugctl & 0xFFFFFFFFULL);
    hi = (uint32_t)(debugctl >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0x1D9));

    return 0;
}

/* Read LBR entries from MSRs */
static int perf_branch_read_lbr(void)
{
    if (!perf_branch.enabled)
        return -EINVAL;

    /* Read LBR stack on current CPU */
    /* LBR entries are in MSR 0x40 - 0x5F (32 entries on modern CPUs) */
    perf_branch.count = 0;

    /* Check LBR format in MSR 0x1C8 (IA32_PERF_CAPABILITIES) */
    /* For simplicity, read first 16 entries */
    for (int i = 0; i < 16; i++) {
        uint32_t from_lo, from_hi, to_lo, to_hi;
        uint32_t msr_from = 0x40 + i * 2;
        uint32_t msr_to = 0x41 + i * 2;

        __asm__ volatile("rdmsr" : "=a"(from_lo), "=d"(from_hi) : "c"(msr_from));
        __asm__ volatile("rdmsr" : "=a"(to_lo), "=d"(to_hi) : "c"(msr_to));

        if (perf_branch.count < PERF_LBR_MAX_ENTRIES) {
            perf_branch.entries[perf_branch.count].from =
                ((uint64_t)from_hi << 32) | from_lo;
            perf_branch.entries[perf_branch.count].to =
                ((uint64_t)to_hi << 32) | to_lo;
            perf_branch.entries[perf_branch.count].flags = 0;
            perf_branch.count++;
        }

        perf_branch.total_branches_sampled++;
    }

    return perf_branch.count;
}

/* Get the last N LBR entries */
static int perf_branch_get_entries(struct perf_lbr_entry *entries, int max)
{
    int n = (perf_branch.count < max) ? perf_branch.count : max;
    memcpy(entries, perf_branch.entries,
           (size_t)n * sizeof(struct perf_lbr_entry));
    return n;
}

static void perf_branch_init(void)
{
    memset(&perf_branch, 0, sizeof(perf_branch));
    kprintf("[OK] perf branch stack (LBR) sampling\n");
}

/* ── perf_branch_read: Read LBR data into user buffer ───────────────── */
static int perf_branch_read(void *data, size_t *len)
{
    if (!data || !len) return -EINVAL;

    /* Use the already-implemented LBR reading functions */
    int count = perf_branch_read_lbr();
    if (count < 0) return count;

    size_t entry_size = sizeof(struct perf_lbr_entry);
    size_t total = (size_t)count * entry_size;
    if (total > *len) total = *len;

    int entries_to_copy = (int)(total / entry_size);
    int n = perf_branch_get_entries((struct perf_lbr_entry *)data, entries_to_copy);
    *len = (size_t)n * entry_size;

    kprintf("[perf] perf_branch_read: read %d LBR entries\n", n);
    return n;
}
