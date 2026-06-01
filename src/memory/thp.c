/* thp.c — Transparent Huge Pages tracking */

#include "thp.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"

#define THP_MAX_PAGES 64

struct thp_entry {
    uint64_t virt_addr;    /* Virtual address of the huge page */
    uint64_t phys_addr;    /* Physical address of the huge page */
    int state;             /* THP_RAW, THP_SPLIT, or THP_PARTIAL */
    int present;
};

static struct thp_entry thp_entries[THP_MAX_PAGES];
static int thp_entry_count = 0;
static int thp_enabled = 0;
static uint64_t thp_total = 0;
static uint64_t thp_merged = 0;
static uint64_t thp_split = 0;

void thp_init(void) {
    memset(thp_entries, 0, sizeof(thp_entries));
    thp_entry_count = 0;
    thp_enabled = 1;
    thp_total = 0;
    thp_merged = 0;
    thp_split = 0;
    kprintf("[mem] THP (Transparent Huge Pages) tracking initialized\n");
}

void thp_set_enabled(int enabled) {
    thp_enabled = enabled;
    kprintf("[mem] THP %s\n", enabled ? "enabled" : "disabled");
}

int thp_is_enabled(void) {
    return thp_enabled;
}

int thp_track_hugepage(uint64_t virt_addr, uint64_t phys_addr) {
    if (!thp_enabled) return -1;
    if (thp_entry_count >= THP_MAX_PAGES) return -1;

    /* Check for duplicate */
    for (int i = 0; i < thp_entry_count; i++) {
        if (thp_entries[i].virt_addr == virt_addr && thp_entries[i].present)
            return 0; /* Already tracked */
    }

    struct thp_entry *entry = &thp_entries[thp_entry_count++];
    entry->virt_addr = virt_addr;
    entry->phys_addr = phys_addr;
    entry->state = THP_RAW;
    entry->present = 1;
    thp_total++;
    return 0;
}

void thp_untrack_hugepage(uint64_t virt_addr) {
    for (int i = 0; i < thp_entry_count; i++) {
        if (thp_entries[i].virt_addr == virt_addr && thp_entries[i].present) {
            thp_entries[i].present = 0;
            thp_total--;
            return;
        }
    }
}

int thp_split_hugepage(uint64_t virt_addr) {
    for (int i = 0; i < thp_entry_count; i++) {
        if (thp_entries[i].virt_addr == virt_addr && thp_entries[i].present) {
            if (thp_entries[i].state == THP_RAW) {
                thp_entries[i].state = THP_SPLIT;
                thp_split++;
                return 512; /* 2 MB / 4 KB = 512 pages */
            }
            return 0;
        }
    }
    return -1;
}

uint64_t thp_get_total_pages(void)  { return thp_total; }
uint64_t thp_get_merged_pages(void) { return thp_merged; }
uint64_t thp_get_split_pages(void)  { return thp_split; }
