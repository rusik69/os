#ifndef CET_H
#define CET_H

#include "types.h"

/* ── Intel CET (Control-flow Enforcement Technology) — Shadow Stack (B6)
 *
 * Per-task shadow stack for ROP protection.
 * Compatible with Intel CET specification (vol. 2, 3).
 *
 * CPUID check: leaf 7, subleaf 0, ECX bit 7 = CET_SS supported.
 *
 * MSRs:
 *   IA32_U_CET    (0x6A0) — user-mode CET config
 *   IA32_PL3_SSP  (0x6A2) — current shadow stack pointer (PL3)
 *   IA32_PL0_SSP  (0x6A4) — supervisor shadow stack pointer
 *   IA32_INTERRUPT_SSP_TABLE (0x6A8)
 *
 * Shadow Stack Token (at base of each shadow stack):
 *   Bits 63:3 — Address of the shadow stack page (matching bits)
 *   Bit 1     — 1 (busy) / 0 (free)
 *   Bit 0     — 1 (valid token)
 */

/* CET MSR addresses */
#define MSR_IA32_U_CET               0x6A0ULL
#define MSR_IA32_PL3_SSP             0x6A2ULL
#define MSR_IA32_PL2_SSP             0x6A4ULL
#define MSR_IA32_PL1_SSP             0x6A5ULL
#define MSR_IA32_PL0_SSP             0x6A6ULL
#define MSR_IA32_INTERRUPT_SSP_TABLE 0x6A8ULL

/* IA32_U_CET flags */
#define CET_SHSTK_EN      (1ULL << 0)   /* Shadow stack enabled */
#define CET_WR_SHSTK_EN   (1ULL << 1)   /* WRSS (Write Shadow Stack) enabled */
#define CET_ENDBR_EN      (1ULL << 2)   /* ENDBRANCH tracking */
#define CET_LEG_IW_EN     (1ULL << 3)   /* Legacy indirect branch tracking */
#define CET_NO_TRACK_EN   (1ULL << 4)   /* No-track prefix */
#define CET_SUPPRESS_DIS  (1ULL << 5)   /* Suppress DIS (disable CET) */
#define CET_DISABLE_IW    (1ULL << 10)  /* Disable indirect branch tracking */
#define CET_WAIT_ENDBR    (1ULL << 31)  /* Wait-for-ENDBRANCH mode */

/* Shadow stack token format */
#define CET_SS_TOKEN_MASK   (~0x7ULL)
#define CET_SS_TOKEN_BUSY   (1ULL << 1)
#define CET_SS_TOKEN_VALID  (1ULL << 0)

#define CET_SS_TOKEN(addr)  ((uint64_t)(addr) | CET_SS_TOKEN_VALID)
#define CET_SS_BUSY_TOKEN(addr) ((uint64_t)(addr) | CET_SS_TOKEN_VALID | CET_SS_TOKEN_BUSY)

/* Shadow stack configuration */
#define CET_SHSTK_SIZE     4096       /* One page per-task shadow stack */
#define CET_SHSTK_GUARD      64       /* Guard space at top of shadow stack */

/* Per-task shadow stack */
struct cet_shadow_stack {
    uint64_t  ssp;            /* Current shadow stack pointer (virtual) */
    uint64_t  base;           /* Base of shadow stack page (virtual) */
    uint64_t  phys_base;      /* Physical address of shadow stack page */
    uint32_t  size;           /* Size in bytes (usually PAGE_SIZE) */
    uint32_t  token_offset;   /* Offset of busy token in page */
    int       enabled;        /* Whether CET is active for this task */
};

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize CET subsystem — check CPUID for hardware support.
 * Returns 0 on success, -EOPNOTSUPP if CET not available. */
int  cet_init(void);

/* Enable CET shadow stack for a specific task.
 * Allocates a shadow stack page and configures IA32_U_CET MSR.
 * Returns 0 on success, negative errno on failure. */
int  cet_enable_per_task(struct cet_shadow_stack *sstk);

/* Disable CET shadow stack for a task.
 * Clears IA32_U_CET and frees the shadow stack page. */
void cet_disable(struct cet_shadow_stack *sstk);

/* Check if CET is supported on this CPU */
int  cet_is_supported(void);

/* Called during context switch to switch shadow stack pointer */
void cet_switch_task(struct cet_shadow_stack *sstk);

#endif /* CET_H */
