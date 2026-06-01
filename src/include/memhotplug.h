#ifndef MEMHOTPLUG_H
#define MEMHOTPLUG_H

#include "types.h"

/* Memory hotplug framework */

/* Maximum number of memory sections that can be hotplugged */
#define MEMHP_MAX_SECTIONS 32
#define MEMHP_SECTION_SIZE (128 * 1024 * 1024) /* 128 MB per section */

/* Memory section state */
enum memhp_state {
    MEMHP_ONLINE,
    MEMHP_OFFLINE,
    MEMHP_GOING_ONLINE,
    MEMHP_GOING_OFFLINE,
};

/* Memory section descriptor */
struct memhp_section {
    uint64_t base_addr;
    uint64_t size;
    enum memhp_state state;
    int present;
};

/* Initialize memory hotplug framework */
void memhp_init(void);

/* Hot-add a memory region */
int memhp_add_region(uint64_t base, uint64_t size);

/* Hot-remove a memory region */
int memhp_remove_region(uint64_t base);

/* Bring a memory section online */
int memhp_online_section(int section_id);

/* Take a memory section offline */
int memhp_offline_section(int section_id);

/* Get number of present sections */
int memhp_get_section_count(void);

/* Get section info */
struct memhp_section *memhp_get_section(int section_id);

#endif /* MEMHOTPLUG_H */
