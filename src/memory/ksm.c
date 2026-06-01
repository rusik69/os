/* ksm.c — Kernel Same-page Merging */

#include "ksm.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"

/* KSM tracking structures */
#define KSM_MAX_PAGES 1024

struct ksm_page {
    uint64_t phys_addr;    /* Physical address of the page */
    uint64_t hash;         /* Content hash (for quick comparison) */
    int merged;            /* Whether this page has been merged */
    int refcount;          /* Reference count if merged */
};

static struct ksm_page ksm_pages[KSM_MAX_PAGES];
static int ksm_page_count = 0;
static int ksm_enabled = 0;
static uint64_t ksm_merged_pages = 0;
static uint64_t ksm_unmergeable_pages = 0;
static uint64_t ksm_scan_count = 0;

/* Simple hash: XOR of all 64-bit words */
static uint64_t ksm_hash_page(uint64_t phys_addr) {
    if (!phys_addr) return 0;
    uint64_t *data = (uint64_t *)PHYS_TO_VIRT(phys_addr);
    uint64_t hash = 0;
    for (int i = 0; i < 512; i++) { /* 4096 / 8 = 512 */
        hash ^= data[i];
    }
    return hash;
}

/* Compare two physical pages for equality */
static int ksm_pages_equal(uint64_t phys_a, uint64_t phys_b) {
    if (phys_a == phys_b) return 1;
    uint64_t *a = (uint64_t *)PHYS_TO_VIRT(phys_a);
    uint64_t *b = (uint64_t *)PHYS_TO_VIRT(phys_b);
    for (int i = 0; i < 512; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

void ksm_init(void) {
    memset(ksm_pages, 0, sizeof(ksm_pages));
    ksm_page_count = 0;
    ksm_enabled = 0;
    ksm_merged_pages = 0;
    ksm_unmergeable_pages = 0;
    ksm_scan_count = 0;
    kprintf("[mem] KSM (Kernel Same-page Merging) initialized\n");
}

void ksm_set_enabled(int enabled) {
    ksm_enabled = enabled;
    kprintf("[mem] KSM %s\n", enabled ? "enabled" : "disabled");
}

int ksm_is_enabled(void) {
    return ksm_enabled;
}

int ksm_register_region(uint64_t addr, size_t size) {
    if (ksm_page_count + size / 4096 > KSM_MAX_PAGES)
        return -1;

    uint64_t phys = VIRT_TO_PHYS(addr);
    for (size_t i = 0; i < size; i += 4096) {
        struct ksm_page *kp = &ksm_pages[ksm_page_count++];
        kp->phys_addr = phys + i;
        kp->hash = ksm_hash_page(kp->phys_addr);
        kp->merged = 0;
        kp->refcount = 1;
    }
    return 0;
}

int ksm_unregister_region(uint64_t addr) {
    uint64_t phys = VIRT_TO_PHYS(addr);
    for (int i = 0; i < ksm_page_count; i++) {
        if (ksm_pages[i].phys_addr == phys) {
            /* Unmerge if needed */
            ksm_pages[i] = ksm_pages[--ksm_page_count];
            return 0;
        }
    }
    return -1;
}

void ksm_scan_cycle(void) {
    if (!ksm_enabled || ksm_page_count < 2) return;

    ksm_scan_count++;

    for (int i = 0; i < ksm_page_count - 1; i++) {
        if (ksm_pages[i].merged) continue;
        for (int j = i + 1; j < ksm_page_count; j++) {
            if (ksm_pages[j].merged) continue;

            /* Quick hash check first */
            if (ksm_pages[i].hash != ksm_pages[j].hash)
                continue;

            /* Full comparison */
            if (ksm_pages_equal(ksm_pages[i].phys_addr, ksm_pages[j].phys_addr)) {
                /* Merge: keep one page, point others to it */
                ksm_pages[j].merged = 1;
                ksm_pages[j].phys_addr = ksm_pages[i].phys_addr;
                ksm_pages[i].refcount++;
                ksm_merged_pages++;
            } else {
                ksm_unmergeable_pages++;
            }
        }
    }
}

uint64_t ksm_get_merged_pages(void)       { return ksm_merged_pages; }
uint64_t ksm_get_unmergeable_pages(void)  { return ksm_unmergeable_pages; }
uint64_t ksm_get_scan_count(void)         { return ksm_scan_count; }
