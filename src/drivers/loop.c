#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "loop.h"
#include "string.h"
#include "spinlock.h"
#define MAX_LOOP 4
static int loop_devices[MAX_LOOP];
static spinlock_t loop_lock;
void loop_init(void) {
    spinlock_init(&loop_lock);
    memset(loop_devices, 0, sizeof(loop_devices));
    kprintf("[OK] Loop device subsystem initialized\n");
}
int loop_create(void) {
    spinlock_acquire(&loop_lock);
    for (int i = 0; i < MAX_LOOP; i++) {
        if (loop_devices[i] == 0) { loop_devices[i] = 1; spinlock_release(&loop_lock); return i; }
    }
    spinlock_release(&loop_lock);
    return -1;
}
int loop_destroy(int idx) {
    if (idx < 0 || idx >= MAX_LOOP) return -1;
    spinlock_acquire(&loop_lock);
    loop_devices[idx] = 0;
    spinlock_release(&loop_lock);
    return 0;
}
