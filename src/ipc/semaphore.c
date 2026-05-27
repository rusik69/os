/* semaphore.c — Kernel counting semaphore (spin+yield) */
#include "semaphore.h"
#include "scheduler.h"

#define SEM_MAX 32

struct sem_entry {
    volatile int count;
    int in_use;
};

static struct sem_entry sems[SEM_MAX];

int sem_init(int count) {
    for (int i = 0; i < SEM_MAX; i++) {
        if (!sems[i].in_use) {
            sems[i].in_use = 1;
            sems[i].count  = count;
            return i;
        }
    }
    return -1;
}

void sem_wait(int id) {
    if (id < 0 || id >= SEM_MAX || !sems[id].in_use) return;
    for (;;) {
        __asm__ volatile("cli");
        if (sems[id].count > 0) {
            sems[id].count--;
            __asm__ volatile("sti");
            return;
        }
        __asm__ volatile("sti");
        scheduler_yield();
    }
}

void sem_post(int id) {
    if (id < 0 || id >= SEM_MAX || !sems[id].in_use) return;
    sems[id].count++;
}

void sem_destroy(int id) {
    if (id < 0 || id >= SEM_MAX) return;
    sems[id].in_use = 0;
    sems[id].count  = 0;
}
