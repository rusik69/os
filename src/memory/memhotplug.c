/* memhotplug.c — Memory hotplug framework */

#include "memhotplug.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "err.h"

static struct memhp_section sections[MEMHP_MAX_SECTIONS];
static int section_count = 0;

void __init memhp_init(void) {
    memset(sections, 0, sizeof(sections));
    kprintf("[MEM] Memory hotplug framework initialized (%d max sections, %lu MB each)\n",
            MEMHP_MAX_SECTIONS, (unsigned long)(MEMHP_SECTION_SIZE / (1024 * 1024)));
}

int memhp_add_region(uint64_t base, uint64_t size) {
    if (section_count >= MEMHP_MAX_SECTIONS) {
        kprintf("[MEM] memhp: max sections reached\n");
        return -ENOSPC;
    }

    struct memhp_section *sec = &sections[section_count];
    sec->base_addr = base;
    sec->size = size;
    sec->state = MEMHP_OFFLINE;
    sec->present = 1;

    section_count++;
    kprintf("[MEM] memhp: added region 0x%lx (+%lu MB)\n",
            (unsigned long)base, (unsigned long)(size / (1024 * 1024)));
    return 0;
}

int memhp_remove_region(uint64_t base) {
    for (int i = 0; i < section_count; i++) {
        if (sections[i].base_addr == base && sections[i].present) {
            if (sections[i].state == MEMHP_ONLINE) {
                kprintf("[MEM] memhp: cannot remove online region 0x%lx\n", (unsigned long)base);
                return -EBUSY;
            }
            sections[i].present = 0;
            sections[i].state = MEMHP_OFFLINE;
            kprintf("[MEM] memhp: removed region 0x%lx\n", (unsigned long)base);
            return 0;
        }
    }
    return -ENOENT;
}

int memhp_online_section(int section_id) {
    if (section_id < 0 || section_id >= section_count)
        return -EINVAL;
    struct memhp_section *sec = &sections[section_id];
    if (!sec->present) return -ENOENT;

    sec->state = MEMHP_GOING_ONLINE;
    /* Add pages to PMM by freeing each page in the region */
    pmm_add_free_frames(sec->base_addr, sec->size);
    sec->state = MEMHP_ONLINE;
    kprintf("[MEM] memhp: section %d online (0x%lx, %lu MB)\n",
            section_id, (unsigned long)sec->base_addr,
            (unsigned long)(sec->size / (1024 * 1024)));
    return 0;
}

int memhp_offline_section(int section_id) {
    if (section_id < 0 || section_id >= section_count)
        return -EINVAL;
    struct memhp_section *sec = &sections[section_id];
    if (!sec->present || sec->state != MEMHP_ONLINE)
        return -EINVAL;

    sec->state = MEMHP_GOING_OFFLINE;
    /* Migrate pages out (simplified: just mark as offline) */
    sec->state = MEMHP_OFFLINE;
    kprintf("[MEM] memhp: section %d offline (0x%lx)\n",
            section_id, (unsigned long)sec->base_addr);
    return 0;
}

int memhp_get_section_count(void) {
    return section_count;
}

struct memhp_section *memhp_get_section(int section_id) {
    if (section_id < 0 || section_id >= section_count)
        return ERR_PTR(-EINVAL);
    return &sections[section_id];
}
#include "module.h"
module_init(memhp_init);

/* ── memhotplug_add ────────────────────────────────────── */
static int memhotplug_add(uint64_t phys_addr, size_t size)
{
    if (size == 0) return -EINVAL;
    if (phys_addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (size & (PAGE_SIZE - 1)) return -EINVAL;

    kprintf("[memhotplug] memhotplug_add: 0x%llx +%llu MB\n",
            (unsigned long long)phys_addr, (unsigned long long)(size / (1024 * 1024)));

    /* Add to hotplug tracking */
    if (section_count >= MEMHP_MAX_SECTIONS) {
        kprintf("[memhotplug] memhotplug_add: max sections reached\n");
        return -ENOMEM;
    }

    struct memhp_section *sec = &sections[section_count];
    sec->base_addr = phys_addr;
    sec->size = size;
    sec->state = MEMHP_OFFLINE;
    sec->present = 1;
    section_count++;

    /* Bring memory online: add pages to PMM */
    sec->state = MEMHP_GOING_ONLINE;
    pmm_add_free_frames(phys_addr, size);
    sec->state = MEMHP_ONLINE;

    kprintf("[memhotplug] memhotplug_add: region 0x%llx (+%llu MB) online\n",
            (unsigned long long)phys_addr, (unsigned long long)(size / (1024 * 1024)));
    return 0;
}

/* ── memhotplug_remove ─────────────────────────────────── */
static int memhotplug_remove(uint64_t phys_addr, size_t size)
{
    if (size == 0) return -EINVAL;
    if (phys_addr & (PAGE_SIZE - 1)) return -EINVAL;

    kprintf("[memhotplug] memhotplug_remove: 0x%llx +%llu MB\n",
            (unsigned long long)phys_addr, (unsigned long long)(size / (1024 * 1024)));

    /* Find and offline the region */
    for (int i = 0; i < section_count; i++) {
        if (sections[i].base_addr == phys_addr && sections[i].present) {
            if (sections[i].state == MEMHP_ONLINE) {
                sections[i].state = MEMHP_GOING_OFFLINE;
                sections[i].state = MEMHP_OFFLINE;
            }
            sections[i].present = 0;
            kprintf("[memhotplug] memhotplug_remove: region 0x%llx removed\n",
                    (unsigned long long)phys_addr);
            return 0;
        }
    }

    kprintf("[memhotplug] memhotplug_remove: region 0x%llx not found\n",
            (unsigned long long)phys_addr);
    return -ENOENT;
}

/* ── memhotplug_notify ─────────────────────────────────── */
static void memhotplug_notify(unsigned long event, void *data)
{
    kprintf("[memhotplug] memhotplug_notify: event=%lu\n", event);
    (void)data;
}

/* ── memhotplug_status ─────────────────────────────────── */
static int memhotplug_status(char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0) return -EINVAL;

    int total = 0;
    int online = 0;
    int offline = 0;
    uint64_t total_size = 0;

    for (int i = 0; i < section_count; i++) {
        if (!sections[i].present) continue;
        total++;
        total_size += sections[i].size;
        if (sections[i].state == MEMHP_ONLINE)
            online++;
        else
            offline++;
    }

    int n = snprintf(buf, bufsize,
                     "Memory Hotplug Status:\n"
                     "  Sections: %d total, %d online, %d offline\n"
                     "  Total size: %llu MB\n"
                     "  Regions:\n",
                     total, online, offline,
                     (unsigned long long)(total_size / (1024 * 1024)));

    for (int i = 0; i < section_count && (size_t)n < bufsize; i++) {
        if (!sections[i].present) continue;
        n += snprintf(buf + n, bufsize - (size_t)n,
                      "    [%d] base=0x%llx size=%llu MB state=%s\n",
                      i,
                      (unsigned long long)sections[i].base_addr,
                      (unsigned long long)(sections[i].size / (1024 * 1024)),
                      sections[i].state == MEMHP_ONLINE ? "online" : "offline");
    }

    return (n < (int)bufsize) ? n : (int)bufsize - 1;
}
