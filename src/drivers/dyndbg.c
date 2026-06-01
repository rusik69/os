#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "dyndbg.h"
#include "string.h"
#define MAX_DEBUG_SITES 64
static struct {
    const char *name;
    int enabled;
} debug_sites[MAX_DEBUG_SITES];
static int debug_count = 0;
void dyndbg_init(void) {
    memset(debug_sites, 0, sizeof(debug_sites));
    kprintf("[OK] Dynamic debug initialized\n");
}
int dyndbg_register(const char *name) {
    if (debug_count >= MAX_DEBUG_SITES) return -1;
    debug_sites[debug_count].name = name;
    debug_sites[debug_count].enabled = 1;
    return debug_count++;
}
void dyndbg_enable(const char *name) {
    for (int i = 0; i < debug_count; i++)
        if (strcmp(debug_sites[i].name, name) == 0) debug_sites[i].enabled = 1;
}
void dyndbg_disable(const char *name) {
    for (int i = 0; i < debug_count; i++)
        if (strcmp(debug_sites[i].name, name) == 0) debug_sites[i].enabled = 0;
}
int dyndbg_enabled(const char *name) {
    for (int i = 0; i < debug_count; i++)
        if (debug_sites[i].enabled && strcmp(debug_sites[i].name, name) == 0) return 1;
    return 0;
}
