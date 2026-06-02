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

/*
 * ── Lockdep — Cross-Release Lock Dependency Validator ──────────────
 *
 * Features:
 *   1. Dependency graph tracking (which locks are held when a lock is acquired)
 *   2. Full deadlock detection via DFS cycle detection on the dependency graph
 *   3. Cross-release (LIFO) enforcement: locks must be released in reverse order
 *   4. Sleeping-while-atomic detection: warn if mutex acquired while holding spinlock
 *   5. Double-lock detection
 *   6. Unlock-without-hold detection
 *   7. Lock leak detection at process exit
 */

/* ── State ────────────────────────────────────────────────────────── */

static struct lock_class lock_classes[LOCKDEP_MAX_LOCKS];
static int class_count = 0;

/* Held lock stack for the current (preempted) context */
static struct held_lock held_locks[LOCKDEP_MAX_DEPTH];
static int held_count = 0;

/* Global sequence counter for LIFO check */
static int global_acquire_seq = 0;

/* Per-CPU spinlock nesting counter — needed because we may not have
 * full per-CPU data support.  This is a simplified global; on SMP
 * each CPU should have its own via an array indexed by CPU ID. */
static int spinlock_nest_count_cpu[64]; /* max 64 CPUs */

int spinlock_nest_count = 0;  /* exported for fast access */

/* ── Forward declarations ────────────────────────────────────────── */

static struct lock_class *find_or_create_class(uint64_t addr,
                                                const char *name,
                                                int type);
static int dfs_detect_cycle(struct lock_class *start,
                            uint64_t target_addr,
                            int *visited,
                            int depth);

/* ── Spinlock owner tracking ─────────────────────────────────────── */

#define SPINLOCK_OWNER_HASH_SIZE 64

struct spinlock_owner_entry {
    spinlock_t *lock;
    int           cpu_id;
    uint32_t      pid;
    uint64_t      caller_rip;
    uint64_t      acquire_tick;
    int           valid;
};

static struct spinlock_owner_entry
    spinlock_owners[SPINLOCK_OWNER_HASH_SIZE];

static int spinlock_owner_hash(spinlock_t *lock) {
    uint64_t addr = (uint64_t)lock;
    return (int)((addr ^ (addr >> 6)) % SPINLOCK_OWNER_HASH_SIZE);
}

void spinlock_register_owner(spinlock_t *lock, uint64_t caller_rip) {
    if (!lock) return;

    int idx = spinlock_owner_hash(lock);
    int start = idx;

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

    spinlock_owners[start].lock = lock;
    spinlock_owners[start].cpu_id = smp_get_cpu_id();
    spinlock_owners[start].pid = process_get_current()
                                  ? process_get_current()->pid : 0;
    spinlock_owners[start].caller_rip = caller_rip;
    spinlock_owners[start].acquire_tick = timer_get_ticks();
    spinlock_owners[start].valid = 1;
}

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

void spinlock_detect_lockup(spinlock_t *lock, uint64_t spin_count) {
    int cpu = smp_get_cpu_id();
    uint64_t now = timer_get_ticks();

    kprintf("\n============================================\n");
    kprintf("=== SPINLOCK LOCKUP DETECTED ===\n");
    kprintf("============================================\n");
    kprintf("CPU %d spinning on lock %p (spin count: %llu)\n",
            cpu, (void *)lock, (unsigned long long)spin_count);

    /* Show elapsed time if timer is available */
    if (now > 0) {
        kprintf("Current tick: %llu\n", (unsigned long long)now);
    }

    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    uint64_t ret_addr = rbp ? ((uint64_t *)rbp)[1] : 0;
    if (ret_addr) {
        kprintf("Spinning caller RIP: 0x%llx", (unsigned long long)ret_addr);
        if (ret_addr >= 0xFFFF800000000000ULL)
            kprintf(" (%s)", kallsyms_lookup(ret_addr));
        kprintf("\n");
    }

    struct spinlock_owner_entry *owner = spinlock_find_owner(lock);
    if (owner) {
        uint64_t hold_time = now > owner->acquire_tick
                             ? (now - owner->acquire_tick) * 1000ULL / TIMER_FREQ
                             : 0;
        kprintf("Lock OWNER: CPU=%d PID=%u (acquired at tick %llu, held for %llu ms)\n",
                owner->cpu_id, (unsigned int)owner->pid,
                (unsigned long long)owner->acquire_tick,
                (unsigned long long)hold_time);
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

    struct process *cur = process_get_current();
    if (cur) {
        kprintf("Current process: %s (pid=%u, state=%d)\n",
                cur->name ? cur->name : "?",
                cur->pid, (int)cur->state);
    }

    int lock_val = *lock;
    kprintf("Lock value: %d (0=free, 1=held)\n", lock_val);

    /* Dump control registers for diagnostic context */
    {
        uint64_t cr0, cr2, cr3, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        kprintf("CR0: 0x%lx  CR2: 0x%lx  CR3: 0x%lx  CR4: 0x%lx\n",
                (unsigned long)cr0, (unsigned long)cr2,
                (unsigned long)cr3, (unsigned long)cr4);
    }

    kprintf("Stack backtrace of spinning CPU %d:\n", cpu);
    dump_stack();
    kprintf("============================================\n");
}

/* ── CPU-local spinlock nesting (for sleeping-while-atomic) ──────── */

void lockdep_spinlock_acquired(void) {
    int cpu = smp_get_cpu_id();
    if (cpu >= 0 && cpu < 64)
        spinlock_nest_count_cpu[cpu]++;
    spinlock_nest_count++;
}

void lockdep_spinlock_released(void) {
    int cpu = smp_get_cpu_id();
    if (cpu >= 0 && cpu < 64) {
        if (spinlock_nest_count_cpu[cpu] > 0)
            spinlock_nest_count_cpu[cpu]--;
    }
    if (spinlock_nest_count > 0)
        spinlock_nest_count--;
}

int lockdep_holding_spinlock(void) {
    int cpu = smp_get_cpu_id();
    if (cpu >= 0 && cpu < 64)
        return spinlock_nest_count_cpu[cpu] > 0;
    return spinlock_nest_count > 0;
}

/* ── Panic notifier ──────────────────────────────────────────────── */

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

/* ── Lock class management ───────────────────────────────────────── */

static const char *lock_type_name(int type) {
    switch (type) {
    case LOCK_TYPE_SPINLOCK: return "spinlock";
    case LOCK_TYPE_MUTEX:    return "mutex";
    case LOCK_TYPE_RWSEM:    return "rwsem";
    case LOCK_TYPE_RCU:      return "rcu";
    default:                 return "unknown";
    }
}

static struct lock_class *find_or_create_class(uint64_t addr,
                                                const char *name,
                                                int type) {
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
    lc->type = type;
    lc->dep_count = 0;
    lc->child_count = 0;
    return lc;
}

/* ── DFS cycle detection on the dependency graph ─────────────────── */

/*
 * Recursive DFS traversal of the lock dependency graph.
 * Returns 1 if `target_addr` is reachable from `start` (cycle found).
 * `visited` is an array of lock_class indices already visited.
 * `depth` is the current recursion depth.
 */
static int dfs_detect_cycle(struct lock_class *start,
                            uint64_t target_addr,
                            int *visited,
                            int depth)
{
    if (depth >= LOCKDEP_MAX_LOCKS)
        return 0; /* bail out to avoid infinite recursion */

    /* Check if any child of `start` equals target_addr */
    for (int c = 0; c < start->child_count; c++) {
        if (start->children[c] == target_addr) {
            /* Cycle found! Dump the chain */
            kprintf("lockdep: DEADLOCK CYCLE DETECTED! "
                    "(depth %d, target 0x%llx)\n",
                    depth, (unsigned long long)target_addr);

            /* Find the class for the target */
            for (int i = 0; i < class_count; i++) {
                if (lock_classes[i].in_use &&
                    lock_classes[i].addr == target_addr) {
                    kprintf("  -> %s (0x%llx) [%s]\n",
                            lock_classes[i].name ? lock_classes[i].name : "?",
                            (unsigned long long)lock_classes[i].addr,
                            lock_type_name(lock_classes[i].type));
                    break;
                }
            }
            return 1;
        }
    }

    /* Recurse through children */
    for (int c = 0; c < start->child_count; c++) {
        uint64_t child_addr = start->children[c];

        /* Check visited to avoid revisiting */
        int already = 0;
        for (int v = 0; v < depth; v++) {
            if (visited[v] >= 0 && visited[v] < class_count) {
                if (lock_classes[visited[v]].addr == child_addr) {
                    already = 1;
                    break;
                }
            }
        }
        if (already) continue;

        /* Find the class for this child */
        struct lock_class *child_lc = NULL;
        for (int i = 0; i < class_count; i++) {
            if (lock_classes[i].in_use && lock_classes[i].addr == child_addr) {
                child_lc = &lock_classes[i];
                break;
            }
        }
        if (!child_lc) continue;

        /* Dump chain as we go */
        kprintf("lockdep:   %s (0x%llx) -> ",
                lock_type_name(start->type),
                (unsigned long long)start->addr);

        visited[depth] = (int)(child_lc - lock_classes);
        if (dfs_detect_cycle(child_lc, target_addr, visited, depth + 1))
            return 1;
    }

    return 0;
}

/* ── Cross-release (LIFO) validation ─────────────────────────────── */

/*
 * Verify that locks are released in strict reverse acquisition order.
 * Returns 0 on success, 1 on cross-release violation.
 */
static int check_lifo_release(uint64_t lock_addr, const char *name, int type)
{
    if (held_count <= 0) {
        /* Should not happen — unlock without hold is caught elsewhere */
        return 0;
    }

    /* LIFO: the lock being released must be the most recently acquired */
    struct held_lock *top = &held_locks[held_count - 1];

    if (top->addr != lock_addr) {
        kprintf("lockdep: CROSS-RELEASE VIOLATION!\n");
        kprintf("  Releasing '%s' (0x%llx) [%s] but most recently held is "
                "'%s' (0x%llx) [%s] (seq %d)\n",
                name ? name : "?",
                (unsigned long long)lock_addr,
                lock_type_name(type),
                top->name ? top->name : "?",
                (unsigned long long)top->addr,
                lock_type_name(top->type),
                top->acquire_seq);

        /* Find the correct position in the held stack */
        int found = -1;
        for (int i = held_count - 1; i >= 0; i--) {
            if (held_locks[i].addr == lock_addr) {
                found = i;
                break;
            }
        }

        if (found >= 0) {
            kprintf("  '%s' was acquired at seq %d (position %d from bottom)\n",
                    name ? name : "?",
                    held_locks[found].acquire_seq, found);
            kprintf("  Locks held between acquisition and release:\n");
            for (int i = found + 1; i < held_count; i++) {
                kprintf("    [%d] '%s' (0x%llx) [%s] seq=%d\n",
                        i,
                        held_locks[i].name ? held_locks[i].name : "?",
                        (unsigned long long)held_locks[i].addr,
                        lock_type_name(held_locks[i].type),
                        held_locks[i].acquire_seq);
            }
        }

        /* Dump caller stack for debugging */
        dump_stack();
        return 1;
    }

    return 0;
}

/* ── core lock_acquire / lock_release ────────────────────────────── */

void lock_acquire(const char *name, uint64_t lock_addr, int type)
{
    if (!name) name = "?";

    struct lock_class *lc = find_or_create_class(lock_addr, name, type);
    if (!lc) return;

    /* ── Sleeping-while-atomic check ── */
    if (type == LOCK_TYPE_MUTEX && lockdep_holding_spinlock()) {
        kprintf("lockdep: SLEEPING WHILE ATOMIC!\n");
        kprintf("  Trying to acquire mutex '%s' (0x%llx) while holding a spinlock\n",
                name, (unsigned long long)lock_addr);
        kprintf("  Spinlock nest count: %d (CPU %d)\n",
                spinlock_nest_count, smp_get_cpu_id());

        /* Dump the held locks */
        kprintf("  Held locks at this point:\n");
        for (int i = 0; i < held_count; i++) {
            kprintf("    [%d] %s (0x%llx) [%s] seq=%d\n",
                    i,
                    held_locks[i].name ? held_locks[i].name : "?",
                    (unsigned long long)held_locks[i].addr,
                    lock_type_name(held_locks[i].type),
                    held_locks[i].acquire_seq);
        }
        dump_stack();
    }

    /* ── Double-lock check ── */
    for (int i = 0; i < held_count; i++) {
        if (held_locks[i].addr == lock_addr) {
            WARN_ON(1);
            kprintf("lockdep: DOUBLE-LOCK of '%s' (%016llx) [%s] by pid=%u\n",
                    name, (unsigned long long)lock_addr,
                    lock_type_name(type),
                    process_get_current() ? process_get_current()->pid : 0);
            return;
        }
    }

    /*
     * ── Dependency recording & cross-release validation ──
     *
     * For every lock already held, record dependency edges:
     *   held_lock -> this_lock   (in `deps` of this lock's class)
     *   this_lock -> held_lock   (in `children` of held lock's class)
     *
     * Then run DFS to detect if this new edge creates a cycle.
     */
    for (int i = 0; i < held_count; i++) {
        uint64_t from = held_locks[i].addr;

        /* Record: held_lock -> this_lock (dependency) */
        int found = 0;
        for (int d = 0; d < lc->dep_count; d++) {
            if (lc->deps[d] == from) { found = 1; break; }
        }
        if (!found && lc->dep_count < LOCKDEP_MAX_LOCKS) {
            lc->deps[lc->dep_count++] = from;
        }

        /* Record: this_lock -> held_lock (child) on the held lock's class */
        struct lock_class *held_lc = find_or_create_class(
            from, held_locks[i].name, held_locks[i].type);
        if (held_lc) {
            int child_found = 0;
            for (int c = 0; c < held_lc->child_count; c++) {
                if (held_lc->children[c] == lock_addr) {
                    child_found = 1;
                    break;
                }
            }
            if (!child_found && held_lc->child_count < LOCKDEP_MAX_LOCKS) {
                held_lc->children[held_lc->child_count++] = lock_addr;
            }

            /*
             * DFS cycle detection:
             * Check if there's already a path from this_lock ->
             * held_lock (i.e., if we're adding an edge that creates a cycle)
             *
             * Note: we need to check if we can reach `from` starting from
             * `lc` via the current dependency graph.  But the graph is
             * small enough to do a full DFS from the held lock looking
             * for the new lock.
             */
            if (held_lc->child_count > 1 || lc->dep_count > 1) {
                int visited[LOCKDEP_MAX_LOCKS];
                for (int v = 0; v < LOCKDEP_MAX_LOCKS; v++)
                    visited[v] = -1;
                visited[0] = (int)(held_lc - lock_classes);

                if (dfs_detect_cycle(held_lc, lock_addr, visited, 1)) {
                    kprintf("lockdep: Cycle details: new edge '%s' (0x%llx) -> "
                            "'%s' (0x%llx) creates a deadlock risk!\n",
                            held_locks[i].name ? held_locks[i].name : "?",
                            (unsigned long long)held_locks[i].addr,
                            name, (unsigned long long)lock_addr);
                    dump_stack();
                }
            }
        }
    }

    /* ── Push onto held stack ── */
    int seq = __sync_fetch_and_add(&global_acquire_seq, 1);
    if (held_count < LOCKDEP_MAX_DEPTH) {
        held_locks[held_count].addr = lock_addr;
        held_locks[held_count].name = name;
        held_locks[held_count].type = type;
        held_locks[held_count].acquire_seq = seq;
        held_count++;
    }
}

void lock_release(const char *name, uint64_t lock_addr, int type)
{
    if (!name) name = "?";

    /* ── Cross-release (LIFO) check ── */
    check_lifo_release(lock_addr, name, type);

    /* ── Find and remove from held stack ── */
    for (int i = held_count - 1; i >= 0; i--) {
        if (held_locks[i].addr == lock_addr) {
            /* Shift remaining locks down */
            for (int j = i; j < held_count - 1; j++) {
                held_locks[j] = held_locks[j + 1];
            }
            held_count--;
            return;
        }
    }

    /* Lock was not held — potential unlock imbalance */
    WARN_ON(1);
    kprintf("lockdep: releasing unheld lock '%s' (%016llx) [%s] by pid=%u\n",
            name ? name : "?",
            (unsigned long long)lock_addr,
            lock_type_name(type),
            process_get_current() ? process_get_current()->pid : 0);
}

/* ── Public API ──────────────────────────────────────────────────── */

int lockdep_check_circular(uint64_t from_addr, uint64_t to_addr)
{
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

void lockdep_dump(void)
{
    kprintf("Lockdep state:\n");
    kprintf("  Classes: %d\n", class_count);
    for (int i = 0; i < class_count; i++) {
        if (!lock_classes[i].in_use) continue;
        kprintf("  [%d] %s (%016llx) [%s] deps=%d children=%d\n",
                i,
                lock_classes[i].name ? lock_classes[i].name : "?",
                (unsigned long long)lock_classes[i].addr,
                lock_type_name(lock_classes[i].type),
                lock_classes[i].dep_count,
                lock_classes[i].child_count);
        for (int d = 0; d < lock_classes[i].dep_count; d++) {
            kprintf("       depends on -> %016llx\n",
                    (unsigned long long)lock_classes[i].deps[d]);
        }
        for (int c = 0; c < lock_classes[i].child_count; c++) {
            kprintf("       is held before -> %016llx\n",
                    (unsigned long long)lock_classes[i].children[c]);
        }
    }
    kprintf("  Held by current CPU: %d\n", held_count);
    for (int i = 0; i < held_count; i++) {
        kprintf("    [%d] %s (%016llx) [%s] seq=%d\n",
                i,
                held_locks[i].name ? held_locks[i].name : "?",
                (unsigned long long)held_locks[i].addr,
                lock_type_name(held_locks[i].type),
                held_locks[i].acquire_seq);
    }

    /* Spinlock owner table */
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

    /* Spinlock nesting */
    int cpu = smp_get_cpu_id();
    kprintf("  Spinlock nest count: %d (CPU %d)\n",
            cpu >= 0 && cpu < 64 ? spinlock_nest_count_cpu[cpu] : 0,
            cpu);
}

void lockdep_check_exit(void)
{
    if (held_count > 0) {
        kprintf("lockdep: process exiting with %d locks held!\n", held_count);
        for (int i = 0; i < held_count; i++) {
            kprintf("  still held: '%s' (%016llx) [%s] seq=%d\n",
                    held_locks[i].name ? held_locks[i].name : "?",
                    (unsigned long long)held_locks[i].addr,
                    lock_type_name(held_locks[i].type),
                    held_locks[i].acquire_seq);
        }
    }
}

void lockdep_init(void)
{
    for (int i = 0; i < LOCKDEP_MAX_LOCKS; i++)
        lock_classes[i].in_use = 0;
    class_count = 0;
    held_count = 0;
    global_acquire_seq = 0;

    /* Clear spinlock nesting counters */
    for (int i = 0; i < 64; i++)
        spinlock_nest_count_cpu[i] = 0;
    spinlock_nest_count = 0;

    /* Initialize spinlock owner tracking */
    memset(spinlock_owners, 0, sizeof(spinlock_owners));

    /* Register panic notifier to release spinlocks on panic */
    spinlock_panic_nb.notifier_call = spinlock_panic_callback;
    spinlock_panic_nb.next = NULL;
    notifier_chain_register(NOTIFIER_PANIC, &spinlock_panic_nb);

    kprintf("[OK] Lockdep initialized (cross-release checking, DFS cycle "
            "detection, sleeping-while-atomic guard)\n");
}
