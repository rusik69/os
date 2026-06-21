/* invpcid.c — INVPCID instruction support for TLB management */

#include "cpu_features.h"
#include "cpu.h"
#include "printf.h"

static int invpcid_available = 0;

/* INVPCID descriptor (12 bytes: 8-byte address + 4-byte PCID) */
struct invpcid_desc {
    uint64_t addr;
    uint64_t pcid; /* Actually only 12 bits, but pad to 16 bits works */
} __attribute__((packed));

int invpcid_init(void) {
    int rax, rbx, rcx, rdx;

    /* Check CPUID leaf 7, subleaf 0 for INVPCID (EBX bit 10) */
    __asm__ volatile("cpuid" : "=a"(rax), "=b"(rbx), "=c"(rcx), "=d"(rdx) : "a"(7), "c"(0));

    if (!(rbx & CPUID_7_EBX_INVPCID)) {
        kprintf("[cpu] INVPCID not supported\n");
        return -1;
    }

    /* Enable INVPCID in CR4 (bit 10) */
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_INVPCID;
    /* Enable PCID in CR4 (bit 17) — tags TLB entries per-process */
    cr4 |= CR4_PCIDE;
    write_cr4(cr4);

    invpcid_available = 1;
    kprintf("[cpu] INVPCID + PCID enabled (CR4 bits 10, 17)\n");
    return 0;
}

/* INVPCID type codes:
 * 0 = INVPCID_TYPE_INDIVIDUAL_ADDR — invalidate TLB entry for a single address + PCID
 * 1 = INVPCID_TYPE_SINGLE_STR — invalidate all entries for a given PCID
 * 2 = INVPCID_TYPE_ALL_NON_GLOBAL — invalidate all non-global entries
 * 3 = INVPCID_TYPE_ALL_INCL_GLOBAL — invalidate all entries including global
 */

void invpcid_flush_all(void) {
    if (!invpcid_available) {
        /* Fallback: full TLB flush via CR3 reload */
        uint64_t cr3 = read_cr3();
        write_cr3(cr3);
        return;
    }
    struct invpcid_desc desc = {0, 0};
    /* Type 3: invalidate all entries, including global */
    __asm__ volatile("invpcid %0, %1"
                     :
                     : "m"(desc), "r"(3ULL)
                     : "memory");
}

void invpcid_flush_single(uint64_t addr) {
    if (!invpcid_available) {
        /* Fallback: invlpg */
        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
        return;
    }
    struct invpcid_desc desc = {addr, 0};
    /* Type 0: invalidate for single address (PCID=0) */
    __asm__ volatile("invpcid %0, %1"
                     :
                     : "m"(desc), "r"(0ULL)
                     : "memory");
}

int invpcid_is_available(void) {
    return invpcid_available;
}

void invpcid_flush_pcid(uint64_t pcid) {
    if (!invpcid_available) {
        /* Fallback: full TLB flush via CR3 reload */
        uint64_t cr3 = read_cr3();
        write_cr3(cr3);
        return;
    }
    struct invpcid_desc desc = {0, pcid & 0xFFF};
    /* Type 1: flush all entries for the given PCID */
    __asm__ volatile("invpcid %0, %1"
                     :
                     : "m"(desc), "r"(1ULL)
                     : "memory");
}

/* ── Stub: invpcid_flush_all_nonglobals ─────────────────────────────── */
int invpcid_flush_all_nonglobals(void)
{
    kprintf("[invpcid] invpcid_flush_all_nonglobals: not yet implemented\n");
    return 0;
}
