#ifndef PAT_H
#define PAT_H

#include "types.h"

/* PAT MSR */
#define MSR_PAT   0x277

/* PAT entry types (4-bit each) */
#define PAT_UC     0x00  /* Uncacheable */
#define PAT_WC     0x01  /* Write-Combining */
#define PAT_WT     0x04  /* Write-Through */
#define PAT_WP     0x05  /* Write-Protected */
#define PAT_WB     0x06  /* Write-Back */
#define PAT_UCM    0x07  /* Uncacheable Minus */

/* PAT index positions in the MSR (8 entries, 4 bits each) */
#define PAT_IDX0   0
#define PAT_IDX1   1
#define PAT_IDX2   2
#define PAT_IDX3   3
#define PAT_IDX4   4
#define PAT_IDX5   5
#define PAT_IDX6   6
#define PAT_IDX7   7

/* PAT entry value in MSR */
#define PAT_MSR_ENTRY(type, idx) ((uint64_t)(type) << ((idx) * 8))

/* Default PAT: WB, WT, UC-, UC, WB, WT, UC-, UC */
#define PAT_DEFAULT_MSR ( \
    PAT_MSR_ENTRY(PAT_WB, 0) | \
    PAT_MSR_ENTRY(PAT_WT, 1) | \
    PAT_MSR_ENTRY(PAT_UCM, 2) | \
    PAT_MSR_ENTRY(PAT_UC, 3) | \
    PAT_MSR_ENTRY(PAT_WB, 4) | \
    PAT_MSR_ENTRY(PAT_WT, 5) | \
    PAT_MSR_ENTRY(PAT_UCM, 6) | \
    PAT_MSR_ENTRY(PAT_UC, 7))

/* Page table PAT bit positions (x86-64 page table entries) */
#define PAGE_PAT_BIT  7   /* Bit 7 in PTE = PAT */
#define PAGE_PCD_BIT  4   /* Bit 4 in PTE = Cache Disable */
#define PAGE_PWT_BIT  3   /* Bit 3 in PTE = Write-Through */

/* API */
int  pat_init(void);
uint64_t pat_read(void);
int  pat_write(uint64_t pat_value);
int  pat_get_entry(int index, uint8_t *type);
int  pat_set_entry(int index, uint8_t type);
int  pat_is_supported(void);

#endif /* PAT_H */
