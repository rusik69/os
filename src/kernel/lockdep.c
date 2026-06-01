#define KERNEL_INTERNAL
#include "types.h"
#include "lockdep.h"
#include "printf.h"
#include "process.h"
#include "panic.h"
#include "smp.h"
#include "timer.h"
#include "string.h"
#include "kallsyms.h"
#include "notifier.h"

/* ── Lock class tracking (existing) ────────────────────────────── */
static struct lock_class lock_classes[LOCKDEP_MAX_LOCKS];
static int class_count = 0;

/* Per-CPU lock depth tracking (simple, non-scalable for now) */
static uint64_t held_locks[LOCKDEP_MAX_DEPTH];
static int      held_count = 0;

/* ── Spinlock owner tracking (new) ─────────────────────────────── */

/* Spinlock owner hash table — tracks who holds each lock for diagnostics */
#define SPINLOCK_OWNER_HASH_SIZE 64

struct spinlock_owner_entry {
    spinlock_t *lock;          /* address of the lock word */
    int           cpu_id;        /* CPU holding the lock */
    uint32_t      pid;           /* process holding the lock */
    uint64_t      caller_rip;    /* instruction that last acquired the lock */
    uint64_t      acquire_tick;  /* tick when lock was acquired */
    int           valid;
};

static struct spinlock_owner_entry
    spinlock_owners[SPINLOCK_OWNER_HASH_SIZE];
static int spinlock_owner_init_done = 0;

/* Simple hash: lock address bits [6..11] mixed with [0..5] */
static int spinlock_owner_hash(spinlock_t *lock) {
    uint64_t addr = (uint64_t)lock;
    return (int)((addr ^ (addr >> 6)) % SPINLOCK_OWNER_HASH_SIZE);
}

/*
 * Register ownership of a spinlock after successful acquire.
 * Overwrites any stale entry for the same lock (shouldn't happen
 * if release was called, but handles re-acquire gracefully).
 */
void spinlock_register_owner(spinlock_t *lock, uint64_t caller_rip) {
    if (!lock) return;

    int idx = spinlock_owner_hash(lock);
    int start = idx;

    /* Find the slot for this lock */
    do {
        if (!spinlock_owners[idx].valid || spinlock_owners[idx].lock == lock) {
            spinlock_owners[idx].lock = lock;
            spinlock_owners[idx].cpu_id = smp_get_cpu_id();
            spinlock_owners[idx].pid = process_get_current()
                                        ? process_get_current()->pid : 0;
            spinlock_owners[idx].caller_rip = caller_rip;
            spinlock_owners[idx].acquire_tick = timer_get_ticks();
            spinlock_owners[idx].valid = 1;
            return;
        }
        idx = (idx + 1) % SPINLOCK_OWNER_HASH_SIZE;
    } while (idx != start);

    /* Hash table full — overwrite the first slot */
    spinlock_owners[start].lock = lock;
    spinlock_owners[start].cpu_id = smp_get_cpu_id();
    spinlock_owners[start].pid = process_get_current()
                                  ? process_get_current()->pid : 0;
    spinlock_owners[start].caller_rip = caller_rip;
    spinlock_owners[start].acquire_tick = timer_get_ticks();
    spinlock_owners[start].valid = 1;
}

/*
 * Clear ownership of a spinlock on release.
 */
void spinlock_unregister_owner(spinlock_t *lock) {
    if (!lock) return;

    int idx = spinlock_owner_hash(lock);
    int start = idx;

    do {
        if (spinlock_owners[idx].valid && spinlock_owners[idx].lock == lock) {
            spinlock_owners[idx].valid = 0;
            spinlock_owners[idx].lock = NULL;
            return;
        }
        idx = (idx + 1) % SPINLOCK_OWNER_HASH_SIZE;
    } while (idx != start);
}

/*
 * Find the owner entry for a lock (for diagnostics).
 * Returns NULL if not found.
 */
static struct spinlock_owner_entry *spinlock_find_owner(spinlock_t *lock) {
    if (!lock) return NULL;

    int idx = spinlock_owner_hash(lock);
    int start = idx;

    do {
        if (spinlock_owners[idx].valid && spinlock_owners[idx].lock == lock)
            return &spinlock_owners[idx];
        idx = (idx + 1) % SPINLOCK_OWNER_HASH_SIZE;
    } while (idx != start);

    return NULL;
}

/*
 * Called from spinlock_acquire() when a thread has been spinning
 * for SPINLOCK_LOCKUP_THRESHOLD iterations.  Dumps diagnostic
 * information to help debug the lockup.
 */
void spinlock_detect_lockup(spinlock_t *lock, uint64_t spin_count) {
    int cpu = smp_get_cpu_id();

    kprintf("\n============================================\n");
    kprintf("=== SPINLOCK LOCKUP DETECTED ===\n");
    kprintf("============================================\n");
    kprintf("CPU %d spinning on lock %p (spin count: %llu)\n",
            cpu, (void *)lock,
            (unsigned long long)spin_count);

    /* Dump caller info */
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    uint64_t ret_addr = rbp ? ((uint64_t *)rbp)[1] : 0;
    if (ret_addr) {
        kprintf("Spinning caller RIP: 0x%llx", (unsigned long long)ret_addr);
        if (ret_addr >= 0xFFFF800000000000ULL)
            kprintf(" (%s)", kallsyms_lookup(ret_addr));
        kprintf("\n");
    }

    /* Dump lock owner if known */
    struct spinlock_owner_entry *owner = spinlock_find_owner(lock);
    if (owner) {
        kprintf("Lock OWNER: CPU=%d PID=%u (acquired at tick %llu)\n",
                owner->cpu_id, (unsigned int)owner->pid,
                (unsigned long long)owner->acquire_tick);
        if (owner->caller_rip) {
            kprintf("Owner acquire RIP: 0x%llx",
                    (unsigned long long)owner->caller_rip);
            if (owner->caller_rip >= 0xFFFF800000000000ULL)
                kprintf(" (%s)", kallsyms_lookup(owner->caller_rip));
            kprintf("\n");
        }
    } else {
        kprintf("Lock owner: unknown (not tracked or already released)\n");
    }

    /* Dump current process */
    struct process *cur = process_get_current();
    if (cur) {
        kprintf("Current process: %s (pid=%u, state=%d)\n",
                cur->name ? cur->name : "?",
                cur->pid, (int)cur->state);
    }

    /* Debug: check if the lock value suggests it's actually held */
    int lock_val = *lock;
    kprintf("Lock value: %d (0=free, 1=held)\n", lock_val);

    /* Stack backtrace */
    kprintf("Stack backtrace of spinning CPU %d:\n", cpu);
    dump_stack();

    kprintf("============================================\n");
}

/*
 * Release all tracked spinlocks held by the current CPU.
 * Called from the panic notifier to prevent deadlocks in panic
 * code paths (e.g., if panic() itself needs a lock held by the
 * panicking CPU).
 */
void spinlock_release_all_on_panic(void) {
    int cpu = smp_get_cpu_id();
    int released = 0;

    for (int i = 0; i < SPINLOCK_OWNER_HASH_SIZE; i++) {
        if (spinlock_owners[i].valid &&
            spinlock_owners[i].cpu_id == cpu) {
            spinlock_t *lock = spinlock_owners[i].lock;
            const char *func = "?";

            if (spinlock_owners[i].caller_rip >= 0xFFFF800000000000ULL)
                func = kallsyms_lookup(spinlock_owners[i].caller_rip);

            kprintf("panic: releasing spinlock %p held by CPU %d "
                    "(acquired by %s at tick %llu)\n",
                    (void *)lock, cpu,
                    func ? func : "?",
                    (unsigned long long)spinlock_owners[i].acquire_tick);

            /* Force-release the lock (write 0) */
            __sync_synchronize();
            __sync_lock_release(lock);

            spinlock_owners[i].valid = 0;
            spinlock_owners[i].lock = NULL;
            released++;
        }
    }

    if (released > 0) {
        kprintf("panic: released %d spinlock(s) held by CPU %d\n",
                released, cpu);
    }
}

/* ── Panic notifier ─────────────────────────────────────────────── */

static struct notifier_block spinlock_panic_nb;

static int spinlock_panic_callback(struct notifier_block *nb,
                                    unsigned long action,
                                    void *data) {
    (void)nb;
    (void)action;
    (void)data;

    kprintf("notifier: spinlock panic handler releasing held locks...\n");
    spinlock_release_all_on_panic();
    return 0;
}

/* ── Existing lockdep functions ─────────────────────────────────── */

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

int lockdep_check_circular(uint64_t from_addr, uint64_t to_addr) {
    struct lock_class *from_lc = NULL;
    for (int i = 0; i < class_count; i++) {
        if (lock_classes[i].in_use && lock_classes[i].addr == from_addr) {
            from_lc = &lock_classes[i];
            break;
        }
    }
    if (!from_lc) return 0;

    for (int d = 0; d < from_lc->dep_count; d++) {
        if (from_lc->deps[d] == to_addr) {
            kprintf("lockdep: circular dependency %016llx -> %016llx\n",
                    (unsigned long long)from_addr,
                    (unsigned long long)to_addr);
            return 1;
        }
    }
    return 0;
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

    /* Dump spinlock owner table */
    kprintf("  Spinlock owners:\n");
    int owner_count = 0;
    for (int i = 0; i < SPINLOCK_OWNER_HASH_SIZE; i++) {
        if (!spinlock_owners[i].valid) continue;
        owner_count++;
        kprintf("    [%d] lock=%p CPU=%d PID=%u RIP=0x%llx tick=%llu\n",
                i,
                (void *)spinlock_owners[i].lock,
                spinlock_owners[i].cpu_id,
                (unsigned int)spinlock_owners[i].pid,
                (unsigned long long)spinlock_owners[i].caller_rip,
                (unsigned long long)spinlock_owners[i].acquire_tick);
    }
    if (owner_count == 0)
        kprintf("    (none)\n");
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

    /* Initialize spinlock owner tracking */
    memset(spinlock_owners, 0, sizeof(spinlock_owners));
    spinlock_owner_init_done = 1;

    /* Register panic notifier to release spinlocks on panic */
    spinlock_panic_nb.notifier_call = spinlock_panic_callback;
    spinlock_panic_nb.next = NULL;
    notifier_chain_register(NOTIFIER_PANIC, &spinlock_panic_nb);

    kprintf("[OK] Lockdep initialized (spinlock debug: lockup threshold=%llu, "
            "panic notifier registered)\n",
            (unsigned long long)SPINLOCK_LOCKUP_THRESHOLD);
}
