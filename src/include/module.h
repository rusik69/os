#ifndef MODULE_H
#define MODULE_H

#include "types.h"
#include "list.h"

#define MODULE_MAX 16

/* Module states */
enum module_state {
    MODULE_UNUSED = 0,
    MODULE_LOADED,
    MODULE_ERROR,
};

/* Module entry point signature */
typedef int (*module_entry_t)(void);

/* Module parameter types */
enum module_param_type {
    PARAM_TYPE_INT,
    PARAM_TYPE_CHAR,
    PARAM_TYPE_STRING,
};

/* A named module parameter */
struct kernel_param {
    char name[32];
    enum module_param_type type;
    void *data;           /* pointer to the actual variable */
    int data_len;
    int perm;             /* permissions (0444, 0644, etc.) */
    int (*set_fn)(const char *val, struct kernel_param *kp);
    int (*get_fn)(char *buf, int max, struct kernel_param *kp);
    struct list_head list;
};

/* Module registration entry */
struct kernel_module {
    char           name[32];
    module_entry_t entry;
    enum module_state state;
    struct list_head params;  /* linked list of kernel_param */
    int             param_count;
};

/* Load a kernel module with the given name and entry function. Returns module_id or -1. */
int module_load(const char *name, module_entry_t entry);

/* Unload a previously loaded module by its module_id. */
int module_unload(int module_id);

/* Find a module by name. Returns NULL if not found. */
struct kernel_module *module_find(const char *name);

/* Initialize the kernel module subsystem. */
void module_init(void);

/* Module parameter registration */
int module_add_param(struct kernel_module *mod, const char *name,
                     enum module_param_type type, void *data, int data_len,
                     int perm, int (*set_fn)(const char*, struct kernel_param*),
                     int (*get_fn)(char*, int, struct kernel_param*));

/* Find a parameter in a module by name */
struct kernel_param *module_find_param(struct kernel_module *mod, const char *name);

/* Macro for declaring a simple integer module parameter */
#define module_param(name, type, perm) \
    static struct kernel_param __module_param_##name = { \
        .name = #name, \
        .type = PARAM_TYPE_##type, \
        .data = &name, \
        .data_len = sizeof(name), \
        .perm = perm, \
        .set_fn = NULL, \
        .get_fn = NULL, \
    }; \
    __attribute__((constructor)) static void __register_param_##name(void) { \
        (void)__module_param_##name; \
    }

/* Macro for module parameter with callback functions */
#define module_param_cb(name, set_fn, get_fn) \
    static struct kernel_param __module_param_cb_##name = { \
        .name = #name, \
        .type = PARAM_TYPE_INT, \
        .data = NULL, \
        .data_len = 0, \
        .perm = 0644, \
        .set_fn = set_fn, \
        .get_fn = get_fn, \
    }; \
    __attribute__((constructor)) static void __register_param_cb_##name(void) { \
        (void)__module_param_cb_##name; \
    }

#endif /* MODULE_H */
