#ifndef MODULE_H
#define MODULE_H

#include "types.h"

#define MODULE_MAX 16

/* Module states */
enum module_state {
    MODULE_UNUSED = 0,
    MODULE_LOADED,
    MODULE_ERROR,
};

/* Module entry point signature */
typedef int (*module_entry_t)(void);

/* Module registration entry */
struct kernel_module {
    char           name[32];
    module_entry_t entry;
    enum module_state state;
};

/* Load a kernel module with the given name and entry function. Returns module_id or -1. */
int module_load(const char *name, module_entry_t entry);

/* Unload a previously loaded module by its module_id. */
int module_unload(int module_id);

/* Find a module by name. Returns NULL if not found. */
struct kernel_module *module_find(const char *name);

/* Initialize the kernel module subsystem. */
void module_init(void);

#endif /* MODULE_H */
