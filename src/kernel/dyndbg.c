#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"

/* ── Dynamic debug: enable/disable pr_debug at runtime ───────────────── */

#define DYNDBG_MAX_MODULES 32
#define DYNDBG_NAME_MAX 32

struct dyndbg_module {
    char name[DYNDBG_NAME_MAX];
    int enabled;
};

static struct dyndbg_module dyndbg_modules[DYNDBG_MAX_MODULES];
static int dyndbg_count = 0;
static int dyndbg_initialized = 0;
static int dyndbg_all_enabled = 0;

void dyndbg_init(void) {
    if (dyndbg_initialized) return;
    memset(dyndbg_modules, 0, sizeof(dyndbg_modules));
    dyndbg_initialized = 1;
    kprintf("[OK] dynamic debug initialized\n");
}

/* Enable/disable debug for a specific module */
int dyndbg_control(const char *module, int enable) {
    if (!module) return -EINVAL;
    
    /* Check if already registered */
    for (int i = 0; i < dyndbg_count; i++) {
        if (strcmp(dyndbg_modules[i].name, module) == 0) {
            dyndbg_modules[i].enabled = enable ? 1 : 0;
            kprintf("[dyndbg] %s %s\n", module, enable ? "enabled" : "disabled");
            return 0;
        }
    }
    
    /* Register new module */
    if (dyndbg_count >= DYNDBG_MAX_MODULES) return -ENOSPC;
    strncpy(dyndbg_modules[dyndbg_count].name, module, DYNDBG_NAME_MAX - 1);
    dyndbg_modules[dyndbg_count].name[DYNDBG_NAME_MAX - 1] = '\0';
    dyndbg_modules[dyndbg_count].enabled = enable ? 1 : 0;
    dyndbg_count++;
    kprintf("[dyndbg] registered '%s' %s\n", module, enable ? "enabled" : "disabled");
    return 0;
}

/* Enable all debug */
void dyndbg_enable_all(void) {
    dyndbg_all_enabled = 1;
    for (int i = 0; i < dyndbg_count; i++)
        dyndbg_modules[i].enabled = 1;
    kprintf("[dyndbg] all modules enabled\n");
}

/* Disable all debug */
void dyndbg_disable_all(void) {
    dyndbg_all_enabled = 0;
    for (int i = 0; i < dyndbg_count; i++)
        dyndbg_modules[i].enabled = 0;
    kprintf("[dyndbg] all modules disabled\n");
}

/* Check if a module's debug is enabled */
int dyndbg_enabled(const char *module) {
    if (!module) return dyndbg_all_enabled;
    if (dyndbg_all_enabled) return 1;
    
    for (int i = 0; i < dyndbg_count; i++) {
        if (strcmp(dyndbg_modules[i].name, module) == 0)
            return dyndbg_modules[i].enabled;
    }
    return 0;
}

/* Print dynamic debug status */
void dyndbg_status(void) {
    kprintf("[dyndbg] all=%s, modules: %d\n",
            dyndbg_all_enabled ? "ON" : "OFF", dyndbg_count);
    for (int i = 0; i < dyndbg_count; i++) {
        kprintf("  %s: %s\n", dyndbg_modules[i].name,
                dyndbg_modules[i].enabled ? "ON" : "OFF");
    }
}
