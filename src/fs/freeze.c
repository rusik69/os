#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "freeze.h"
static int fs_frozen = 0;
void freeze_init(void) {
    kprintf("[OK] Filesystem freeze subsystem initialized\n");
}
int freeze_fs(void) {
    if (fs_frozen) return -1;
    fs_frozen = 1;
    kprintf("[freeze] Filesystem frozen\n");
    return 0;
}
int thaw_fs(void) {
    if (!fs_frozen) return -1;
    fs_frozen = 0;
    kprintf("[freeze] Filesystem thawed\n");
    return 0;
}
int is_frozen(void) { return fs_frozen; }
