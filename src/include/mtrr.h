#ifndef MTRR_H
#define MTRR_H

#include "types.h"

/* MTRR memory types */
#define MTRR_TYPE_UC         0   /* Uncacheable */
#define MTRR_TYPE_WC         1   /* Write-Combining */
#define MTRR_TYPE_WT         4   /* Write-Through */
#define MTRR_TYPE_WP         5   /* Write-Protected */
#define MTRR_TYPE_WB         6   /* Write-Back */

/* MTRR MSRs */
#define MSR_MTRR_CAP         0x0FE
#define MSR_MTRR_DEF_TYPE    0x2FF
#define MSR_MTRR_PHYS_BASE0  0x200
#define MSR_MTRR_PHYS_MASK0  0x201
#define MSR_MTRR_PHYS_BASE1  0x202
#define MSR_MTRR_PHYS_MASK1  0x203
#define MSR_MTRR_PHYS_BASE2  0x204
#define MSR_MTRR_PHYS_MASK2  0x205
#define MSR_MTRR_PHYS_BASE3  0x206
#define MSR_MTRR_PHYS_MASK3  0x207
#define MSR_MTRR_PHYS_BASE4  0x208
#define MSR_MTRR_PHYS_MASK4  0x209
#define MSR_MTRR_PHYS_BASE5  0x20A
#define MSR_MTRR_PHYS_MASK5  0x20B
#define MSR_MTRR_PHYS_BASE6  0x20C
#define MSR_MTRR_PHYS_MASK6  0x20D
#define MSR_MTRR_PHYS_BASE7  0x20E
#define MSR_MTRR_PHYS_MASK7  0x20F
#define MSR_MTRR_PHYS_BASE8  0x210
#define MSR_MTRR_PHYS_MASK8  0x211
#define MSR_MTRR_PHYS_BASE9  0x212
#define MSR_MTRR_PHYS_MASK9  0x213

/* MTRR cap bits */
#define MTRR_CAP_VCNT_MASK   0xFF
#define MTRR_CAP_WC          (1U << 8)
#define MTRR_CAP_FIX         (1U << 10)

/* MTRR def type bits */
#define MTRR_DEF_ENABLE      (1U << 11)
#define MTRR_DEF_FIX_ENABLE  (1U << 10)

/* MTRR variable range register */
struct mtrr_range {
    int      valid;
    uint64_t base;
    uint64_t mask;
    int      type;
    int      enabled;
};

/* API */
int  mtrr_init(void);
int  mtrr_get_count(void);
int  mtrr_get(int index, struct mtrr_range *range);
int  mtrr_set(int index, uint64_t base, uint64_t size, int type);
void mtrr_enable(void);
void mtrr_disable(void);
int  mtrr_is_supported(void);
int  mtrr_has_wc(void);

#endif /* MTRR_H */
