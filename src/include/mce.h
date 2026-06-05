#ifndef MCE_H
#define MCE_H

#include "types.h"

/* Forward declaration from idt.h */
struct interrupt_frame;

/* ── MCE MSR definitions ─────────────────────────────────────────── */

/* Global machine check registers */
#define MSR_IA32_MCG_CAP       0x179ULL
#define MSR_IA32_MCG_STATUS    0x17AULL
#define MSR_IA32_MCG_CTL       0x17BULL

/* Per-bank machine check registers (bank i: base + 4*i) */
#define MSR_IA32_MC0_CTL       0x400ULL
#define MSR_IA32_MC0_STATUS    0x401ULL
#define MSR_IA32_MC0_ADDR      0x402ULL
#define MSR_IA32_MC0_MISC      0x403ULL

/* Per-bank CTL2 (threshold-based logging, newer CPUs) */
#define MSR_IA32_MC0_CTL2      0x280ULL

#define MCE_BANK_CTL(base, i)       ((base) + 4ULL * (uint64_t)(i))
#define MCE_BANK_STATUS(base, i)    ((base) + 1ULL + 4ULL * (uint64_t)(i))
#define MCE_BANK_ADDR(base, i)      ((base) + 2ULL + 4ULL * (uint64_t)(i))
#define MCE_BANK_MISC(base, i)      ((base) + 3ULL + 4ULL * (uint64_t)(i))
#define MCE_BANK_CTL2(i)            (MSR_IA32_MC0_CTL2 + (uint64_t)(i))

/* IA32_MCG_CAP bits */
#define MCG_CAP_COUNT_MASK    0xFFULL        /* bits [7:0] = number of banks */
#define MCG_CAP_CTL_P         (1ULL << 8)    /* IA32_MCG_CTL present */
#define MCG_CAP_EXT_P         (1ULL << 9)    /* extended MSRs present */
#define MCG_CAP_CMCI_P        (1ULL << 10)   /* CMCI present */
#define MCG_CAP_LMCE_P        (1ULL << 11)   /* Local MCE present */

/* IA32_MCG_STATUS bits */
#define MCG_STATUS_RIPV       (1ULL << 0)    /* Restart IP valid */
#define MCG_STATUS_EIPV       (1ULL << 1)    /* Error IP valid */
#define MCG_STATUS_MCIP       (1ULL << 2)    /* Machine Check in progress */

/* IA32_MCi_STATUS bits */
#define MC_STATUS_VAL         (1ULL << 63)   /* Valid */
#define MC_STATUS_OVER        (1ULL << 62)   /* Overflow */
#define MC_STATUS_UC          (1ULL << 61)   /* Uncorrectable */
#define MC_STATUS_EN          (1ULL << 60)   /* Enabled */
#define MC_STATUS_MISCV       (1ULL << 59)   /* Misc field valid */
#define MC_STATUS_ADDRV       (1ULL << 58)   /* Address field valid */
#define MC_STATUS_PCC         (1ULL << 57)   /* Processor context corrupted */
#define MC_STATUS_S           (1ULL << 56)   /* Signaled */
#define MC_STATUS_AR          (1ULL << 55)   /* Action required (Intel) */

#define MC_STATUS_ERRCODE_MASK   0xFFFFULL          /* bits [15:0] */
#define MC_STATUS_ERRCODE_SHIFT  0
#define MC_STATUS_MODCODE_MASK   0xFFFF0000ULL      /* bits [31:16] */
#define MC_STATUS_MODCODE_SHIFT  16
#define MC_STATUS_OTHER_MASK     0x00FF000000000000ULL /* bits [52:45] vendor-specific */

/* IA32_MCi_CTL2 bits */
#define MC_CTL2_ERROR_THRESHOLD   0x7FFFULL         /* bits [14:0] */
#define MC_CTL2_CLR_CTR           (1ULL << 29)     /* Clear counter */
#define MC_CTL2_CMCI_EN           (1ULL << 30)     /* Corrected MC interrupt enable */

/* ── MCE severity classification ─────────────────────────────────── */

enum mce_severity {
    MCE_SEV_NO_ERROR    = 0,   /* No valid error */
    MCE_SEV_CORRECTED   = 1,   /* Corrected (recoverable) error */
    MCE_SEV_FATAL       = 2,   /* Fatal — processor context corrupted */
    MCE_SEV_UC          = 3,   /* Uncorrected but maybe recoverable */
    MCE_SEV_DEFERRED    = 4,   /* Deferred error (AMD) */
};

/* ── MCE bank information ────────────────────────────────────────── */

struct mce_bank_info {
    uint32_t    bank_num;
    uint64_t    status;
    uint64_t    addr;
    uint64_t    misc;
    uint64_t    mcg_status;
    enum mce_severity severity;
};

/* ── Public API ──────────────────────────────────────────────────── */

/* Initialize MCE subsystem: register vector 18 handler, enable banks */
void mce_init(void);

/* Machine Check handler — called on #MC (vector 18) */
void mce_handler(struct interrupt_frame *frame);

/* Dump all available MCE bank info (for diagnostics) */
void mce_dump_banks(void);

/* ── MCE injection (Item 396) ────────────────────────────────────── */

/* Initialise the MCE inject debugfs interface.
 * Creates files under /sys/kernel/debug/mce-inject/ for injecting
 * synthetic machine check errors to test the MCE handler. */
void mce_inject_init(void);

#endif /* MCE_H */
