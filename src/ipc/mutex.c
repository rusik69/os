/* mutex.c — Kernel mutex implementation (spin+yield) */
#include "mutex.h"
#include "scheduler.h"

#define MUTEX_MAX 32

struct mutex_entry {
    volatile int locked;
    int in_use;
};

static struct mutex_entry mutexes[MUTEX_MAX];

int mutex_init(void) {
    for (int i = 0; i < MUTEX_MAX; i++) {
        __asm__ volatile("cli");
        if (!mutexes[i].in_use) {
            mutexes[i].in_use  = 1;
            mutexes[i].locked  = 0;
            __asm__ volatile("sti");
            return i;
        }
        __asm__ volatile("sti");
    }
    return -1;
}

void mutex_lock(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return;
    for (;;) {
        __asm__ volatile("cli");
        if (!mutexes[id].locked) {
            mutexes[id].locked = 1;
            __asm__ volatile("sti");
            return;
        }
        __asm__ volatile("sti");
        scheduler_yield();
    }
}

void mutex_unlock(int id) {
    if (id < 0 || id >= MUTEX_MAX || !mutexes[id].in_use) return;
    mutexes[id].locked = 0;
}

void mutex_destroy(int id) {
    if (id < 0 || id >= MUTEX_MAX) return;
    mutexes[id].in_use  = 0;
    mutexes[id].locked  = 0;
}
