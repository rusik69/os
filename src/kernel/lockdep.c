#define KERNEL_INTERNAL
#include "types.h"
#include "lockdep.h"
#include "printf.h"
#include "process.h"
#include "panic.h"

static struct lock_class lock_classes[LOCKDEP_MAX_LOCKS];
static int class_count = 0;

/* Per-CPU lock depth tracking (simple, non-scalable for now) */
static uint64_t held_locks[LOCKDEP_MAX_DEPTH];
static int      held_count = 0;

static struct lock_class *find_or_create_class(uint64_t addr, const char *name) {
    /* Search existing */
    for (int i = 0; i < class_count; i++) {
        if (lock_classes[i].in_use && lock_classes[i].addr == addr)
            return &lock_classes[i];
    }
    /* Create new */
    if (class_count >= LOCKDEP_MAX_LOCKS) {
        kprintf("lockdep: too many lock classes!\n");
        return NULL;
    }
    struct lock_class *lc = &lock_classes[class_count++];
    lc->name = name;
    lc->addr = addr;
    lc->in_use = 1;
    lc->dep_count = 0;
    return lc;
}

void lock_acquire(const char *name, uint64_t lock_addr) {
    if (!name) name = "?";

    struct lock_class *lc = find_or_create_class(lock_addr, name);
    if (!lc) return;

    /* Check for double-lock */
    for (int i = 0; i < held_count; i++) {
        if (held_locks[i] == lock_addr) {
            WARN_ON(1);
            kprintf("lockdep: double-lock of '%s' (%016llx) by pid=%u\n",
                    name, (unsigned long long)lock_addr,
                    process_get_current() ? process_get_current()->pid : 0);
            return;
        }
    }

    /* Record dependencies: every currently-held lock → this lock */
    for (int i = 0; i < held_count; i++) {
        uint64_t from = held_locks[i];
        /* Check if edge already exists */
        int found = 0;
        for (int d = 0; d < lc->dep_count; d++) {
            if (lc->deps[d] == from) { found = 1; break; }
        }
        if (!found && lc->dep_count < LOCKDEP_MAX_LOCKS) {
            lc->deps[lc->dep_count++] = from;
        }

        /* Deadlock detection: check for circular dependency */
        struct lock_class *from_lc = find_or_create_class(from, NULL);
        if (from_lc) {
            for (int d = 0; d < from_lc->dep_count; d++) {
                if (from_lc->deps[d] == lock_addr) {
                    kprintf("lockdep: DEADLOCK risk! '%s'->'%s' cycle detected\n",
                            from_lc->name ? from_lc->name : "?",
                            name);
                    dump_stack();
                }
            }
        }
    }

    /* Push onto held stack */
    if (held_count < LOCKDEP_MAX_DEPTH) {
        held_locks[held_count++] = lock_addr;
    }
}

void lock_release(const char *name, uint64_t lock_addr) {
    (void)name;

    /* Find lock in held stack */
    for (int i = held_count - 1; i >= 0; i--) {
        if (held_locks[i] == lock_addr) {
            /* Shift remaining locks down */
            for (int j = i; j < held_count - 1; j++)
                held_locks[j] = held_locks[j + 1];
            held_count--;
            return;
        }
    }

    /* Lock was not held — potential unlock imbalance */
    WARN_ON(1);
    kprintf("lockdep: releasing unheld lock '%s' (%016llx) by pid=%u\n",
            name ? name : "?", (unsigned long long)lock_addr,
            process_get_current() ? process_get_current()->pid : 0);
}

void lockdep_dump(void) {
    kprintf("Lockdep state:\n");
    kprintf("  Classes: %d\n", class_count);
    for (int i = 0; i < class_count; i++) {
        if (!lock_classes[i].in_use) continue;
        kprintf("  [%d] %s (%016llx) deps=%d\n", i,
                lock_classes[i].name ? lock_classes[i].name : "?",
                (unsigned long long)lock_classes[i].addr,
                lock_classes[i].dep_count);
        for (int d = 0; d < lock_classes[i].dep_count; d++) {
            kprintf("       -> %016llx\n",
                    (unsigned long long)lock_classes[i].deps[d]);
        }
    }
    kprintf("  Held by current CPU: %d\n", held_count);
    for (int i = 0; i < held_count; i++) {
        kprintf("    [%d] %016llx\n", i, (unsigned long long)held_locks[i]);
    }
}

void lockdep_check_exit(void) {
    if (held_count > 0) {
        kprintf("lockdep: process exiting with %d locks held!\n", held_count);
        for (int i = 0; i < held_count; i++) {
            kprintf("  still held: %016llx\n", (unsigned long long)held_locks[i]);
        }
    }
}

void lockdep_init(void) {
    for (int i = 0; i < LOCKDEP_MAX_LOCKS; i++)
        lock_classes[i].in_use = 0;
    class_count = 0;
    held_count = 0;
    kprintf("[OK] Lockdep initialized\n");
}
