/* memhotplug.c — Memory hotplug framework */

#include "memhotplug.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"

static struct memhp_section sections[MEMHP_MAX_SECTIONS];
static int section_count = 0;

void memhp_init(void) {
    memset(sections, 0, sizeof(sections));
    kprintf("[mem] Memory hotplug framework initialized (%d max sections, %llu MB each)\n",
            MEMHP_MAX_SECTIONS, (uint64_t)(MEMHP_SECTION_SIZE / (1024 * 1024)));
}

int memhp_add_region(uint64_t base, uint64_t size) {
    if (section_count >= MEMHP_MAX_SECTIONS) {
        kprintf("[mem] memhp: max sections reached\n");
        return -1;
    }

    struct memhp_section *sec = &sections[section_count];
    sec->base_addr = base;
    sec->size = size;
    sec->state = MEMHP_OFFLINE;
    sec->present = 1;

    section_count++;
    kprintf("[mem] memhp: added region 0x%llx (+%llu MB)\n",
            (uint64_t)base, (uint64_t)(size / (1024 * 1024)));
    return 0;
}

int memhp_remove_region(uint64_t base) {
    for (int i = 0; i < section_count; i++) {
        if (sections[i].base_addr == base && sections[i].present) {
            if (sections[i].state == MEMHP_ONLINE) {
                kprintf("[mem] memhp: cannot remove online region 0x%llx\n", (uint64_t)base);
                return -1;
            }
            sections[i].present = 0;
            sections[i].state = MEMHP_OFFLINE;
            kprintf("[mem] memhp: removed region 0x%llx\n", (uint64_t)base);
            return 0;
        }
    }
    return -1;
}

int memhp_online_section(int section_id) {
    if (section_id < 0 || section_id >= section_count)
        return -1;
    struct memhp_section *sec = &sections[section_id];
    if (!sec->present) return -1;

    sec->state = MEMHP_GOING_ONLINE;
    /* Add pages to PMM by freeing each page in the region */
    pmm_reserve_frames(sec->base_addr, sec->size);
    sec->state = MEMHP_ONLINE;
    kprintf("[mem] memhp: section %d online (0x%llx, %llu MB)\n",
            section_id, (uint64_t)sec->base_addr,
            (uint64_t)(sec->size / (1024 * 1024)));
    return 0;
}

int memhp_offline_section(int section_id) {
    if (section_id < 0 || section_id >= section_count)
        return -1;
    struct memhp_section *sec = &sections[section_id];
    if (!sec->present || sec->state != MEMHP_ONLINE)
        return -1;

    sec->state = MEMHP_GOING_OFFLINE;
    /* Migrate pages out (simplified: just mark as offline) */
    sec->state = MEMHP_OFFLINE;
    kprintf("[mem] memhp: section %d offline (0x%llx)\n",
            section_id, (uint64_t)sec->base_addr);
    return 0;
}

int memhp_get_section_count(void) {
    return section_count;
}

struct memhp_section *memhp_get_section(int section_id) {
    if (section_id < 0 || section_id >= section_count)
        return NULL;
    return &sections[section_id];
}
