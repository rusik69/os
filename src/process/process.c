#define KERNEL_INTERNAL
#include "process.h"

#include "bug.h"
#include "caps.h"
#include "cgroup_namespace.h"
#include "cpu_topology.h"
#include "errno.h"
#include "export.h"
#include "futex.h" /* for futex_robust_list_cleanup() */
#include "heap.h"
#include "ioprio.h"
#include "kcov.h"
#include "kpti.h"
#include "lsm.h"
#include "mnt_namespace.h"
#include "pid_namespace.h"
#include "pmm.h"
#include "printf.h"
#include "sched_attr.h"
#include "scheduler.h"
#include "signal.h"
#include "smp.h"
#include "string.h"
#include "syscall.h" /* for prng_rand64(), syscall_dispatch(), etc. */
#include "sysctl.h"  /* for sysctl_get_hostname() */
#include "timer.h"
#include "uaccess.h"
#include "user_namespace.h"
#include "vmm.h"

/* ── Compile-time struct size assertions ────────────────────────────── */

/*
 * ────────────────────────────────────────────────────────────────────────────
 * Process Lifecycle State Machine
 * ────────────────────────────────────────────────────────────────────────────
 *
 * Each process (struct process) transitions through a well-defined set of
 * states during its lifetime.  The states are defined in include/process.h:
 *
 *   PROCESS_UNUSED  (0) — Slot is free (empty).  No process structure
 *                         is allocated here.  This is the initial state
 *                         of all process_table[] entries at boot.
 *
 *   PROCESS_READY   (1) — Process exists and is eligible to run.  It is
 *                         enqueued in the scheduler's runqueue and will
 *                         be selected by schedule() when its turn comes.
 *                         All freshly created processes start here.
 *
 *   PROCESS_RUNNING (2) — Process is currently executing on a CPU core.
 *                         Only the idle process (PID 0) is in this state
 *                         permanently; user processes oscillate between
 *                         READY and RUNNING as the scheduler dispatches
 *                         and preempts them.
 *
 *   PROCESS_BLOCKED (3) — Process is waiting for an event (I/O completion,
 *                         timer expiry, signal, mutex, semaphore, etc.).
 *                         It is NOT in the scheduler runqueue.  When the
 *                         wait condition is satisfied, the process is
 *                         moved to PROCESS_READY and re-enqueued.
 *
 *   PROCESS_ZOMBIE  (4) — Process has exited (process_exit() called) but
 *                         its parent has not yet called wait4()/waitpid()
 *                         to collect its exit status.  The process holds
 *                         no resources (memory, file descriptors, etc.)
 *                         beyond the process_table[] slot itself, which
 *                         is needed for the parent to read the exit code.
 *                         When the parent reaps it, the slot returns to
 *                         PROCESS_UNUSED.
 *
 *
 * STATE TRANSITION DIAGRAM
 * ────────────────────────
 *
 *     ┌────────────┐
 *     │ UNUSED (0) │◄────────────────────────────┐
 *     └─────┬──────┘   wait/reap recycles slot    │
 *           │                                     │
 *           │ process_create() / fork()           │
 *           ▼                                     │
 *     ┌───────────┐     schedule() picks proc     │
 *     │ READY (1) │──────────────────────────────►│
 *     └─────┬─────┘                               │
 *           │◄────────────────────┐               │
 *           │  preempt/yield      │               │
 *           │                     │               │
 *           ▼                     │               │
 *     ┌─────────────┐            │               │
 *     │ RUNNING (2) │             │               │
 *     └──────┬──────┘             │               │
 *            │                    │               │
 *            │ I/O/sleep/wait     │               │
 *            ▼                    │               │
 *     ┌───────────┐   wakeup     │               │
 *     │ BLOCKED(3)│────────────► │               │
 *     └───────────┘              │               │
 *                                │               │
 *     ┌───────────┐  process_    │               │
 *     │ ZOMBIE (4)│  exit()      │               │
 *     └─────┬─────┘              │               │
 *           │                                     │
 *           │ parent calls wait4()/waitpid()      │
 *           └─────────────────────────────────────┘
 *
 *
 * KEY FUNCTIONS & TRANSITIONS
 * ────────────────────────────
 *
 *   process_create()  ── UNUSED → READY
 *     Allocates a new process slot, assigns a PID, allocates a guarded
 *     kernel stack, and initializes scheduling/resource fields.  The new
 *     process starts in PROCESS_READY and will be scheduled normally.
 *
 *   process_fork()    ── RUNNING → READY  (parent: RUNNING)
 *     Duplicates the calling process.  The child is created in PROCESS_READY;
 *     the parent remains PROCESS_RUNNING.  Both return from the fork with
 *     different return values.
 *
 *   schedule()        ── RUNNING ↔ READY
 *     The scheduler picks the next process from the runqueue.  The currently
 *     running process is moved to PROCESS_READY (unless it has blocked or
 *     exited).  The picked process transitions from READY to RUNNING.
 *
 *   scheduler_wait/sleep/block()  ── RUNNING → BLOCKED
 *     When a process calls a blocking operation (read from empty pipe,
 *     wait on semaphore, sleep(), etc.), it is moved to PROCESS_BLOCKED
 *     and removed from the runqueue.  The scheduler picks another process.
 *
 *   scheduler_wakeup()  ── BLOCKED → READY
 *     When the wait condition is satisfied (I/O arrives, timer fires,
 *     signal is delivered, etc.), the blocked process is returned to
 *     PROCESS_READY and re-enqueued in the runqueue.
 *
 *   process_exit()    ── RUNNING → ZOMBIE
 *     The process terminates.  Resources are freed (memory, file descriptors,
 *     kernel stack, etc.), but the process_table[] slot is preserved with
 *     state = PROCESS_ZOMBIE so the parent can read the exit status.
 *     The parent is notified via SIGCHLD.
 *
 *   wait4()/waitpid()  ── ZOMBIE → UNUSED
 *     The parent calls wait4() to collect the child's exit status.  The
 *     zombie's process_table[] slot is cleared and returned to UNUSED,
 *     making it available for future process_create() calls.  If the
 *     parent exits first, orphaned children are re-parented to init
 *     (PID 1), which periodically reaps them.
 *
 * ORPHANED PROCESS GROUPS
 * ────────────────────────
 *   When a session leader exits, the kernel checks for orphaned process
 *   groups within that session.  An orphaned process group has no member
 *   whose parent is in the same session and process group.  The kernel
 *   delivers SIGHUP followed by SIGCONT to all members of each orphaned
 *   group (POSIX.1 job-control semantics).  See check_orphaned_process_groups().
 *
 * THE IDLE PROCESS (PID 0)
 * ─────────────────────────
 *   PID 0 is the idle process, created at boot in process_init().  It runs
 *   only when no other process is READY.  Idle has state PROCESS_RUNNING
 *   permanently.  It is never allocated via process_create() and never
 *   transitions to any other state.
 * ────────────────────────────────────────────────────────────────────────────
 */

_Static_assert(sizeof(struct process) >= 2048,
               "struct process must be at least 2048 bytes for fixed-size table");
_Static_assert(sizeof(struct cpu_context) == 64, "cpu_context must be 8 x uint64_t (packed)");
_Static_assert(sizeof(struct itimerval) == 16, "itimerval must be 2 x uint64_t");

static struct process process_table[PROCESS_MAX];
extern void user_entry_trampoline(void);
extern void process_entry_trampoline(void);
static struct process *current_process = NULL;

/* PID bitmap allocator: 256 processes → 4 × uint64_t.
 * Bit N ≡ PID N allocated.  Bit 0 always set (idle process).
 * O(1) alloc via __builtin_ctzll on inverted word.
 * Protected by pid_lock to prevent race conditions. */
static uint64_t pid_bitmap[4];
static spinlock_t pid_lock;
#define PID_BITMAP_WORDS 4

static int pid_is_free_in_table(uint32_t pid) {
    if (pid == 0)
        return 0; /* PID 0 is reserved for idle */
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_UNUSED && process_table[i].pid == pid) {
            return 0; /* still in use by another slot */
        }
    }
    return 1;
}

/* Monotonic PID counter for wraparound-based allocation.
 * Incremented on each allocation; when it reaches PROCESS_MAX it wraps
 * to 1, spreading PID reuse across the full range and avoiding immediate
 * reuse of recently-freed PIDs, which could confuse userspace. */
static uint32_t last_pid = 0;

static uint32_t alloc_pid(void) {
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pid_lock, &irq_flags);
    uint32_t pid = (uint32_t)-1;

    /* Search for a free PID starting from (last_pid + 1), wrapping
     * around when we exceed PROCESS_MAX.  PID 0 is reserved for the
     * idle process and is never handed out.  Linear scan — at most
     * PROCESS_MAX iterations (256) — negligible cost for fork. */
    for (uint32_t i = 1; i < PROCESS_MAX; i++) {
        uint32_t candidate = (last_pid + i) % PROCESS_MAX;
        if (candidate == 0)
            continue; /* skip PID 0 */

        int w = (int)(candidate / 64);
        int bit = (int)(candidate % 64);

        if (pid_bitmap[w] & (1ULL << bit))
            continue;

        /* Defense-in-depth: verify the PID is truly free in the
         * process table (the bitmap can drift in edge cases). */
        if (!pid_is_free_in_table(candidate)) {
            pid_bitmap[w] |= (1ULL << bit); /* sync bitmap with reality */
            continue;
        }

        pid_bitmap[w] |= (1ULL << bit);
        pid = candidate;
        break;
    }

    if (pid != (uint32_t)-1) {
        /* Log wraparound event when the counter wraps around */
        if (pid < last_pid && last_pid > 0) {
            kprintf("[pid] Wraparound: PID counter wrapped at %d, "
                    "new PID %u (last was %u)\n",
                    PROCESS_MAX, pid, last_pid);
        }
        last_pid = pid;
    }

    spinlock_irqsave_release(&pid_lock, irq_flags);
    return pid;
}

static void free_pid(uint32_t pid) {
    if (pid >= PROCESS_MAX)
        return;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&pid_lock, &irq_flags);
    int w = pid / 64;
    int bit = pid % 64;
    if (w < 0 || w >= PID_BITMAP_WORDS) {
        spinlock_irqsave_release(&pid_lock, irq_flags);
        return;
    }
    pid_bitmap[w] &= ~(1ULL << bit);
    spinlock_irqsave_release(&pid_lock, irq_flags);
}

/* ── Kernel stack with guard page ───────────────────────────────────── */

/* Kernel-stack cache: freed 33-frame contiguous stack blocks are kept as
 * whole units instead of returning them to the pmm bitmap, where pinned
 * stack frames permanently fragment free runs (observed: 99% fragmentation,
 * largest free run 32 frames — one short of the 33 needed → OOM kills the
 * shell mid-e2e-suite).  Caching the freed block lets the next fork reuse
 * it without a contiguous-bitmap scan. */
#define KSTACK_CACHE_MAX 8
static uint64_t g_kstack_cache[KSTACK_CACHE_MAX]; /* phys base (guard frame) */
static int g_kstack_cache_count;
static spinlock_t g_kstack_cache_lock;

/* Allocate a kernel stack with an unmapped guard page at the bottom.
 * Stack grows downward: [guard (unmapped)][kernel_stack ... stack_top]
 * A stack overflow past kernel_stack will hit the guard page and fault.
 * Returns 0 on success, -1 on failure (all frames freed on error). */
static int alloc_guarded_kernel_stack(struct process *proc) {
    uint64_t guard_phys;
    int from_cache = 0;

    /* Prefer a cached block — no bitmap scan, keeps contiguity intact */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_kstack_cache_lock, &irq_flags);
    if (g_kstack_cache_count > 0) {
        guard_phys = g_kstack_cache[--g_kstack_cache_count];
        from_cache = 1;
    }
    spinlock_irqsave_release(&g_kstack_cache_lock, irq_flags);

    if (!from_cache) {
        uint64_t *phys = pmm_alloc_frames(KERNEL_STACK_TOTAL_PAGES);
        if (unlikely(!phys)) {
            kprintf("[alloc_kernel_stack] pmm_alloc_frames(%d) failed\n", KERNEL_STACK_TOTAL_PAGES);
            return -ENOMEM;
        }
        guard_phys = (uint64_t)phys;
    }
    uint64_t stack_phys = guard_phys + PAGE_SIZE;
    uint64_t guard_vma = (uint64_t)PHYS_TO_VIRT(guard_phys);
    uint64_t stack_vma = (uint64_t)PHYS_TO_VIRT(stack_phys);

    /* Pin every frame of the kernel stack in the page-table bitmap.
     * Kernel stacks live in the physmap (PHYS_TO_VIRT) and the guard
     * page PTE is deliberately unmapped for overflow detection — if
     * compaction ever migrates one of these frames (refcount == 1,
     * MOVABLE pageblock), the physmap hole at the guard PTE makes
     * PHYS_TO_VIRT fault and the stack is corrupted under the live
     * process (observed: #PF in migrate_one_page on a kernel-stack
     * frame after OOM-triggered compaction). */
    for (size_t i = 0; i < KERNEL_STACK_TOTAL_PAGES; i++)
        pmm_mark_pt_frame(guard_phys + i * PAGE_SIZE);

    /* Call vmm_map_page for the guard VMA — this splits the 2MB huge page
     * into 4KB PTEs for the region if needed.  Then unmap just the guard.
     * -EEXIST is fine: the page was already present (identity-mapped),
     * which means the huge page has been split successfully. */
    int map_rc = vmm_map_page(guard_vma, guard_phys, VMM_FLAG_WRITE);
    if (map_rc < 0 && map_rc != -EEXIST) {
        kprintf("[alloc_kernel_stack] vmm_map_page(0x%lx, 0x%lx) failed rc=%d\n", guard_vma,
                guard_phys, map_rc);
        if (from_cache) {
            /* Return the block to the cache — frames stay pinned/allocated */
            uint64_t rflags;
            spinlock_irqsave_acquire(&g_kstack_cache_lock, &rflags);
            if (g_kstack_cache_count < KSTACK_CACHE_MAX)
                g_kstack_cache[g_kstack_cache_count++] = guard_phys;
            else
                from_cache = 0; /* cache full — fall through to free below */
            spinlock_irqsave_release(&g_kstack_cache_lock, rflags);
        }
        if (!from_cache) {
            for (size_t i = 0; i < KERNEL_STACK_TOTAL_PAGES; i++) {
                pmm_unmark_pt_frame(guard_phys + i * PAGE_SIZE);
                pmm_free_frame(guard_phys + i * PAGE_SIZE);
            }
        }
        return -EINVAL;
    }
    vmm_unmap_page(guard_vma);

    proc->guard_page = guard_vma;
    proc->kernel_stack = stack_vma;
    proc->stack_top = stack_vma + KERNEL_STACK_SIZE;
    return 0;
}

/* Free a kernel stack previously allocated by alloc_guarded_kernel_stack.
 * Re-maps the guard page first so freeing its physical frame is safe.
 * The 33-frame block is cached as a whole unit (frames stay pinned and
 * allocated) so the next fork reuses it without a contiguous-bitmap scan. */
static void free_guarded_kernel_stack(struct process *proc) {
    if (!proc->kernel_stack)
        return;

    uint64_t guard_phys = VIRT_TO_PHYS(proc->guard_page);

    /* Re-map guard page so the PTEs are consistent while we free */
    vmm_map_page(proc->guard_page, guard_phys, VMM_FLAG_WRITE);

    /* Cache the whole block — frames remain allocated + PT-pinned, so
     * compaction can't move them and the block keeps its contiguity. */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_kstack_cache_lock, &irq_flags);
    if (g_kstack_cache_count < KSTACK_CACHE_MAX) {
        g_kstack_cache[g_kstack_cache_count++] = guard_phys;
        spinlock_irqsave_release(&g_kstack_cache_lock, irq_flags);
        proc->guard_page = 0;
        proc->kernel_stack = 0;
        proc->stack_top = 0;
        return;
    }
    spinlock_irqsave_release(&g_kstack_cache_lock, irq_flags);

    for (size_t i = 0; i < KERNEL_STACK_TOTAL_PAGES; i++) {
        pmm_unmark_pt_frame(guard_phys + i * PAGE_SIZE);
        pmm_free_frame(guard_phys + i * PAGE_SIZE);
    }

    proc->guard_page = 0;
    proc->kernel_stack = 0;
    proc->stack_top = 0;
}

static inline int process_cap_valid(uint32_t num) {
    return num < PROCESS_SYSCALL_MAX;
}

void process_caps_clear_all(struct process *proc) {
    if (!proc)
        return;
    memset(proc->syscall_caps, 0, sizeof(proc->syscall_caps));
}

void process_caps_allow(struct process *proc, uint32_t num) {
    if (!proc || !process_cap_valid(num))
        return;
    proc->syscall_caps[num / 64] |= (1ULL << (num % 64));
}

void process_caps_allow_all(struct process *proc) {
    if (!proc)
        return;
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++) {
        proc->syscall_caps[i] = ~0ULL;
    }
}

int process_caps_has(const struct process *proc, uint32_t num) {
    if (!proc || !process_cap_valid(num))
        return 0;
    /* Check capability and bounding set */
    if (!(proc->syscall_caps[num / 64] & (1ULL << (num % 64))))
        return 0;
    /* Also check bounding set */
    if (!(proc->cap_bset[num / 64] & (1ULL << (num % 64))))
        return 0;
    return 1;
}

static void process_caps_apply_user_default(struct process *proc) {
    process_caps_clear_all(proc);

    /* Core process/syscall lifecycle */
    process_caps_allow(proc, SYS_READ);
    process_caps_allow(proc, SYS_WRITE);
    process_caps_allow(proc, SYS_OPEN);
    process_caps_allow(proc, SYS_CLOSE);
    process_caps_allow(proc, SYS_EXIT);
    process_caps_allow(proc, SYS_GETPID);
    process_caps_allow(proc, SYS_KILL);
    process_caps_allow(proc, SYS_BRK);
    process_caps_allow(proc, SYS_STAT);
    process_caps_allow(proc, SYS_MKDIR);
    process_caps_allow(proc, SYS_UNLINK);
    process_caps_allow(proc, SYS_TIME);
    process_caps_allow(proc, SYS_YIELD);
    process_caps_allow(proc, SYS_UPTIME);
    process_caps_allow(proc, SYS_WAITPID);
    process_caps_allow(proc, SYS_SLEEP_TICKS);

    /* Process lifecycle: fork/exec/signals — required by init and the
     * shell to spawn children and install handlers. */
    process_caps_allow(proc, SYS_FORK);
    process_caps_allow(proc, SYS_EXECVE);
    process_caps_allow(proc, SYS_SIGNAL);
    process_caps_allow(proc, SYS_DUP);
    process_caps_allow(proc, SYS_DUP2);

    /* Filesystem/VFS through syscall boundary */
    process_caps_allow(proc, SYS_FS_CREATE);
    process_caps_allow(proc, SYS_FS_WRITE);
    process_caps_allow(proc, SYS_FS_READ);
    process_caps_allow(proc, SYS_FS_DELETE);
    process_caps_allow(proc, SYS_FS_LIST);
    process_caps_allow(proc, SYS_FS_STAT);
    process_caps_allow(proc, SYS_FS_STAT_EX);
    process_caps_allow(proc, SYS_FS_CHMOD);
    process_caps_allow(proc, SYS_FS_CHOWN);
    process_caps_allow(proc, SYS_FS_GET_USAGE);
    process_caps_allow(proc, SYS_FS_LIST_NAMES);
    process_caps_allow(proc, SYS_VFS_READ);
    process_caps_allow(proc, SYS_VFS_WRITE);
    process_caps_allow(proc, SYS_VFS_STAT);
    process_caps_allow(proc, SYS_VFS_CREATE);
    process_caps_allow(proc, SYS_VFS_UNLINK);
    process_caps_allow(proc, SYS_VFS_READDIR);
    process_caps_allow(proc, SYS_FD_READ);
    process_caps_allow(proc, SYS_FD_WRITE);

    /* Network stack access */
    process_caps_allow(proc, SYS_NET_PRESENT);
    process_caps_allow(proc, SYS_NET_GET_MAC);
    process_caps_allow(proc, SYS_NET_GET_IP);
    process_caps_allow(proc, SYS_NET_GET_GW);
    process_caps_allow(proc, SYS_NET_GET_MASK);
    process_caps_allow(proc, SYS_NET_DNS);
    process_caps_allow(proc, SYS_NET_PING);
    process_caps_allow(proc, SYS_NET_TRACE);
    process_caps_allow(proc, SYS_NET_UDP_SEND);
    process_caps_allow(proc, SYS_NET_HTTP_GET);
    process_caps_allow(proc, SYS_NET_ARP_LIST);
    /* TCP server syscalls */
    process_caps_allow(proc, SYS_NET_TCP_LISTEN);
    process_caps_allow(proc, SYS_NET_TCP_ACCEPT);
    process_caps_allow(proc, SYS_NET_TCP_SEND_CONN);
    process_caps_allow(proc, SYS_NET_TCP_RECV_CONN);
    process_caps_allow(proc, SYS_NET_TCP_CLOSE_CONN);
    process_caps_allow(proc, SYS_NET_TCP_UNLISTEN);

    /* User program execution helpers */
    process_caps_allow(proc, SYS_ELF_EXEC);
    process_caps_allow(proc, SYS_SCRIPT_EXEC);
    process_caps_allow(proc, SYS_MEMFD_CREATE);

    /* io_uring async I/O syscalls */
    process_caps_allow(proc, SYS_IO_URING_SETUP);
    process_caps_allow(proc, SYS_IO_URING_ENTER);
    process_caps_allow(proc, SYS_IO_URING_REGISTER);

    /* Kernel module loading (insmod) */
    process_caps_allow(proc, SYS_INIT_MODULE);
}

static void process_caps_apply_user_trusted(struct process *proc) {
    process_caps_allow_all(proc);
}

int process_set_cap_profile(struct process *proc, enum process_cap_profile profile) {
    if (!proc)
        return -EINVAL;

    switch (profile) {
    case PROCESS_CAP_PROFILE_NONE:
        process_caps_clear_all(proc);
        break;
    case PROCESS_CAP_PROFILE_USER_DEFAULT:
        process_caps_apply_user_default(proc);
        break;
    case PROCESS_CAP_PROFILE_USER_TRUSTED:
        process_caps_apply_user_trusted(proc);
        break;
    default:
        return -EINVAL;
    }

    proc->cap_profile = (uint8_t)profile;
    return 0;
}

static void rlimit_init_defaults(struct process *proc) {
    /* Default resource limits (RLIM_INFINITY for most) */
    for (int i = 0; i < _RLIMIT_NLIMITS; i++) {
        proc->rlim_cur[i] = ~0ULL;
        proc->rlim_max[i] = ~0ULL;
    }
    /* Set sensible defaults */
    proc->rlim_cur[RLIMIT_NOFILE] = 1024;              /* RLIMIT_NOFILE soft = 1024 */
    proc->rlim_max[RLIMIT_NOFILE] = 4096;              /* RLIMIT_NOFILE hard = 4096 */
    proc->rlim_cur[RLIMIT_NPROC] = 4096;               /* RLIMIT_NPROC  soft = 4096 */
    proc->rlim_max[RLIMIT_NPROC] = 4096;               /* RLIMIT_NPROC  hard = 4096 */
    proc->rlim_cur[RLIMIT_AS] = 1024ULL * 1024 * 1024; /* RLIMIT_AS = 1GB */
    proc->rlim_max[RLIMIT_AS] = 1024ULL * 1024 * 1024;
    proc->rlim_cur[RLIMIT_CORE] = 1024ULL * 1024; /* RLIMIT_CORE = 1MB */
    proc->rlim_max[RLIMIT_CORE] = 1024ULL * 1024;
    proc->rlim_cur[RLIMIT_STACK] = 8ULL * 1024 * 1024; /* RLIMIT_STACK = 8MB */
    proc->rlim_max[RLIMIT_STACK] = 8ULL * 1024 * 1024;
    proc->rlim_cur[RLIMIT_MEMLOCK] = 1024ULL * 64; /* RLIMIT_MEMLOCK = 64KB */
    proc->rlim_max[RLIMIT_MEMLOCK] = 1024ULL * 64;
}

void __init process_init(void) {
    /* Compile-time assertions for critical struct sizes */
    BUILD_BUG_ON(sizeof(struct process) < 1024);
    BUILD_BUG_ON(sizeof(struct cpu_context) < 64);
    BUILD_BUG_ON(sizeof(struct process_fd) < 80);
    BUILD_BUG_ON(sizeof(struct itimerval) < 16);

    memset(process_table, 0, sizeof(process_table));
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    spinlock_init(&pid_lock);
    spinlock_init(&g_kstack_cache_lock);
    pid_bitmap[0] = 1; /* PID 0 (idle) is always allocated */

    /* Initialize per-process fd_table locks */
    for (int i = 0; i < PROCESS_MAX; i++)
        spinlock_init(&process_table[i].fd_table_lock);

    /* Initialize PID namespace subsystem (root namespace) */
    pid_ns_init();

    /* Initialize cgroup namespace subsystem (Item 117) */
    cgroup_ns_init();

    /* Initialize user namespace subsystem (Item 114) */
    user_ns_init();

    /* Create idle process (pid 0) - represents the boot thread */
    process_table[0].pid = 0;
    process_table[0].state = PROCESS_RUNNING;
    process_table[0].name = "idle";
    process_table[0].pending_signals = 0;
    process_table[0].sig_mask = 0;
    process_table[0].is_user = 0;
    process_table[0].pml4 = NULL;
    process_table[0].parent_pid = 0;
    process_table[0].pgid = 0;
    process_table[0].sid = 0;
    process_table[0].exit_code = 0;
    process_table[0].sleep_until = 0;
    process_table[0].is_background = 0;
    process_table[0].is_suspended = 0;
    process_table[0].priority = 1;
    process_table[0].base_priority = 1;
    process_table[0].nice = NICE_DEFAULT;
    process_table[0].cpu_affinity = 0; /* allow any CPU */
    process_table[0].uid = 0;          /* root */
    process_table[0].gid = 0;
    process_table[0].euid = 0;
    process_table[0].egid = 0;
    process_table[0].umask = 0022; /* default: rwxr-xr-x */
    memset(process_table[0].itimers, 0, sizeof(process_table[0].itimers));
    process_table[0].cap_profile = PROCESS_CAP_PROFILE_USER_TRUSTED;
    process_caps_allow_all(&process_table[0]);
    memset(process_table[0].sig_handlers, 0, sizeof(process_table[0].sig_handlers));
    memset(process_table[0].sig_flags, 0, sizeof(process_table[0].sig_flags));
    memset(process_table[0].sig_sa_mask, 0, sizeof(process_table[0].sig_sa_mask));
    process_table[0].sched_policy = SCHED_OTHER;
    process_table[0].alt_stack_sp = NULL;
    process_table[0].alt_stack_size = 0;
    process_table[0].alt_stack_flags = SS_DISABLE;
    process_table[0].personality = 0;
    process_table[0].coredump_enabled = 1;
    process_table[0].dumpable = 1; /* SUID_DUMP_USER */
    memset(process_table[0].proc_comm, 0, 16);
    rlimit_init_defaults(&process_table[0]);
    cap_bset_init(&process_table[0]);
    process_table[0].securebits = 0;
    /* Assign the idle process to the root PID namespace */
    process_table[0].pid_ns = &init_pid_ns;
    process_table[0].ns_pid = 0;
    /* Assign the idle process to the root user namespace */
    process_table[0].user_ns = &init_user_ns;
    process_table[0].landlock_ruleset_ids[0] = -1; /* no landlock restrictions */
    process_table[0].landlock_ruleset_ids[1] = -1;
    process_table[0].landlock_ruleset_ids[2] = -1;
    process_table[0].landlock_ruleset_ids[3] = -1;
    current_process = &process_table[0];

    /* Allocate a guarded kernel stack for the idle process so that
     * stack_top, kernel_stack are valid (not 0) for tss_set_rsp0,
     * syscall stack entries, and stack boundary checks in schedule().
     * The context field will be set naturally on the first context switch. */
    if (alloc_guarded_kernel_stack(&process_table[0]) < 0) {
        kprintf("[!!] Failed to allocate kernel stack for idle process\n");
    }

    /* Give idle a unique stack canary so the global guard is never 0
     * when we context-switch back to idle.  prng_rand64() is safe to
     * call here — rng_init() runs before process_init() in kernel_main. */
    process_table[0].stack_canary = prng_rand64();

    /* ── Initialize CFS and resource tracking fields ── */
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_table[i].vruntime = 0;
        process_table[i].sched_weight = 1024;
        process_table[i].sched_autogroup_id = -1;
        process_table[i].ioprio = IOPRIO_DEFAULT;
        process_table[i].cpu_user = 0;
        process_table[i].cpu_system = 0;
        process_table[i].max_rss = 0;
        process_table[i].swap_pages = 0;
        process_table[i].page_faults = 0;
        process_table[i].signals_received = 0;
        process_table[i].context_switches = 0;
        process_table[i].io_rchar = 0;
        process_table[i].io_wchar = 0;
        process_table[i].io_syscr = 0;
        process_table[i].io_syscw = 0;
        process_table[i].io_read_bytes = 0;
        process_table[i].io_write_bytes = 0;
        process_table[i].stack_watermark = 0;
    }
}

struct process *process_create(void (*entry)(void), const char *name) {
    struct process *proc = NULL;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            proc = &process_table[i];
            break;
        }
    }
    if (!proc)
        return NULL;

    /* Enforce RLIMIT_NPROC: check processes owned by the same UID */
    struct process *cur = process_get_current();
    if (cur) {
        uint64_t nproc_limit = cur->rlim_cur[RLIMIT_NPROC];
        if (nproc_limit != ~0ULL) {
            uint64_t same_user_count = 0;
            for (int i = 0; i < PROCESS_MAX; i++) {
                if (process_table[i].state != PROCESS_UNUSED &&
                    process_table[i].state != PROCESS_ZOMBIE && process_table[i].uid == cur->uid) {
                    same_user_count++;
                }
            }
            if (same_user_count >= nproc_limit)
                return NULL;
        }
    }

    /* Allocate kernel stack with guard page */
    if (alloc_guarded_kernel_stack(proc) < 0)
        return NULL;

    proc->pid = alloc_pid();
    if (proc->pid == (uint32_t)-1) {
        kprintf("[process_create] alloc_pid failed for '%s'\n", name ? name : "?");
        free_guarded_kernel_stack(proc);
        return NULL;
    }
    /* LSM task_alloc hook: security modules initialize per-task state
     * (Smack label, Landlock bindings) for a newly reserved pid. */
    lsm_task_alloc(proc->pid);
    proc->state = PROCESS_READY;
    proc->next = NULL;
    proc->pending_signals = 0;
    proc->sig_mask = 0;
    memset(proc->sig_handlers, 0, sizeof(proc->sig_handlers));
    memset(proc->sig_flags, 0, sizeof(proc->sig_flags));
    memset(proc->sig_sa_mask, 0, sizeof(proc->sig_sa_mask));
    memset(proc->fd_table, 0, sizeof(proc->fd_table));
    proc->is_user = 0;
    proc->user_entry = 0;
    proc->user_rsp = 0;
    proc->pml4 = NULL;
    proc->parent_pid = current_process ? current_process->pid : 0;
    proc->pgid = current_process ? current_process->pgid : proc->pid;
    proc->sid = current_process ? current_process->sid : proc->pid;
    proc->exit_code = 0;
    proc->sleep_until = 0;
    proc->is_background = 0;
    proc->is_suspended = 0;
    proc->priority = 1; /* normal priority */
    proc->nice = NICE_DEFAULT;
    proc->cpu_affinity = 0; /* allow any CPU */
    proc->on_cpu = 0;       /* not yet executing */
    proc->uid = 0;
    proc->gid = 0;
    proc->euid = 0;
    proc->egid = 0;
    proc->ngroups = 0;
    proc->umask = 0022;
    proc->wait_for_pid = 0;
    proc->ticks_remaining = 0; /* set by scheduler on first run */
    proc->last_run_tick = timer_get_ticks();
    /* Inherit cwd from parent */
    if (current_process && current_process->cwd[0])
        strncpy(proc->cwd, current_process->cwd, 63);
    else
        strncpy(proc->cwd, "/", 63);
    proc->cwd[63] = '\0';
    process_set_cap_profile(proc, PROCESS_CAP_PROFILE_USER_TRUSTED);
    proc->sched_policy = SCHED_OTHER;
    proc->alt_stack_sp = NULL;
    proc->alt_stack_size = 0;
    proc->alt_stack_flags = SS_DISABLE;
    proc->personality = 0;
    proc->coredump_enabled = 1;
    proc->dumpable = 1; /* default: dumpable (SUID_DUMP_USER) */
    memset(proc->proc_comm, 0, sizeof(proc->proc_comm));
    strncpy(proc->proc_comm, name, sizeof(proc->proc_comm) - 1);
    proc->proc_comm[sizeof(proc->proc_comm) - 1] = '\0';
    proc->name = proc->proc_comm;
    memset(proc->exe_path, 0, sizeof(proc->exe_path));
    memset(proc->itimers, 0, sizeof(proc->itimers));
    rlimit_init_defaults(proc);
    cap_bset_init(proc);
    proc->landlock_ruleset_ids[0] = -1; /* no landlock restrictions */
    proc->landlock_ruleset_ids[1] = -1;
    proc->landlock_ruleset_ids[2] = -1;
    proc->landlock_ruleset_ids[3] = -1;
    proc->ptracer_pid = 0; /* YAMA: no tracer allowed by default */
    kcov_process_init(proc);

    /* Initialize CPU time accounting */
    proc->utime_ticks = 0;
    proc->stime_ticks = 0;
    proc->start_time_tick = timer_get_ticks();
    proc->cpu_limit_warned_tick = 0;
    proc->nvcsw = 0;
    proc->nivcsw = 0;
    proc->minflt = 0;
    proc->majflt = 0;

    /* CFS vruntime and resource tracking */
    proc->stack_canary = prng_rand64();
    proc->vruntime = 0;
    proc->sched_weight = 1024;
    proc->sched_autogroup_id = -1;
    proc->ioprio = IOPRIO_DEFAULT;
    /* NUMA home node — default to current CPU's NUMA node */
    proc->home_node = numa_home_node();
    proc->cpu_user = 0;

    /* ── UTS namespace: inherit hostname/domainname from parent or global ── */
    {
        const char *src_host =
            current_process ? current_process->ns_hostname : sysctl_get_hostname();
        strncpy(proc->ns_hostname, src_host ? src_host : "localhost",
                sizeof(proc->ns_hostname) - 1);
        proc->ns_hostname[sizeof(proc->ns_hostname) - 1] = '\0';

        const char *default_domain = "(none)";
        const char *src_domain = current_process ? current_process->ns_domainname : default_domain;
        strncpy(proc->ns_domainname, src_domain ? src_domain : default_domain,
                sizeof(proc->ns_domainname) - 1);
        proc->ns_domainname[sizeof(proc->ns_domainname) - 1] = '\0';
    }

    /* ── Time namespace: inherit offsets from parent ──────────────────── */
    {
        proc->timens_mono_offset = current_process ? current_process->timens_mono_offset : 0;
        proc->timens_boottime_offset =
            current_process ? current_process->timens_boottime_offset : 0;
    }

    /* ── PID namespace: inherit from parent (Item 111) ──────────── */
    {
        struct process *parent = current_process;
        if (parent && parent->pid_ns) {
            proc->pid_ns = parent->pid_ns;
            /* If parent is in a different PID namespace, allocate local PID */
            if (proc->pid_ns != &init_pid_ns) {
                proc->ns_pid = pid_ns_alloc_pid(proc->pid_ns);
            } else {
                proc->ns_pid = proc->pid; /* root ns: ns_pid == global pid */
            }
        } else {
            proc->pid_ns = &init_pid_ns;
            proc->ns_pid = proc->pid;
        }
    }

    /* ── Cgroup namespace: inherit from parent (Item 117) ──────────── */
    {
        struct process *parent = current_process;
        if (parent && parent->cgroup_ns) {
            proc->cgroup_ns = cgroup_ns_get(parent->cgroup_ns);
        } else {
            proc->cgroup_ns = NULL; /* root namespace */
        }
    }

    /* ── User namespace: inherit from parent (Item 114) ──────────── */
    {
        struct process *parent = current_process;
        if (parent && parent->user_ns) {
            /* Take a reference on the inherited namespace */
            proc->user_ns = user_ns_get(parent->user_ns);
        } else {
            proc->user_ns = &init_user_ns; /* root namespace */
        }
    }
    proc->cpu_system = 0;
    proc->max_rss = 0;
    proc->swap_pages = 0;
    proc->page_faults = 0;
    proc->signals_received = 0;
    proc->context_switches = 0;
    proc->stack_watermark = 0;

    /* Initialize PELT load tracking */
    pelt_init(&proc->pelt);

    /* Initialize wakee flips heuristic fields */
    proc->last_wakee = NULL;
    proc->wakee_flip_cnt = 0;
    proc->wakee_flip_tick = 0;

    /* Set up initial context on the stack */
    uint64_t *sp = (uint64_t *)(proc->stack_top);

    /* context_switch will pop: r15, r14, r13, r12, rbx, rbp, then ret to rip.
     * For new processes, ret goes to process_entry_trampoline which does sti
     * then jmp r15 (the real entry point). This is needed because schedule()
     * does cli before context_switch. */
    sp -= 7;
    sp[0] = (uint64_t)entry;                    /* r15 = real entry point */
    sp[1] = 0;                                  /* r14 */
    sp[2] = 0;                                  /* r13 */
    sp[3] = 0;                                  /* r12 */
    sp[4] = 0;                                  /* rbx */
    sp[5] = 0;                                  /* rbp */
    sp[6] = (uint64_t)process_entry_trampoline; /* rip = trampoline */

    proc->context = (struct cpu_context *)sp;

    if (scheduler_add(proc) < 0) {
        /* Release the PID and the inherited cgroup-namespace ref taken above */
        free_pid(proc->pid);
        if (proc->cgroup_ns)
            cgroup_ns_put(proc->cgroup_ns);
        /* Release the inherited user-namespace ref taken above */
        if (proc->user_ns)
            user_ns_put(proc->user_ns);
        /* Release the namespace-local PID allocated for a non-root
         * inherited PID namespace above (was leaked before). */
        if (proc->pid_ns && proc->pid_ns != &init_pid_ns && proc->ns_pid > 0)
            pid_ns_free_pid(proc->pid_ns, proc->ns_pid);
        free_guarded_kernel_stack(proc);
        proc->state = PROCESS_UNUSED;
        return NULL;
    }
    return proc;
}

struct process *process_create_user(uint64_t entry, uint64_t user_rsp, uint64_t *pml4,
                                    const char *name) {
    struct process *proc = NULL;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            proc = &process_table[i];
            break;
        }
    }
    if (!proc)
        return NULL;

    /* Allocate kernel stack for syscall handling */
    if (alloc_guarded_kernel_stack(proc) < 0) {
        kprintf("[process_create_user] alloc_guarded_kernel_stack failed\n");
        return NULL;
    }

    proc->pid = alloc_pid();
    if (proc->pid == (uint32_t)-1) {
        kprintf("[process_create_user] alloc_pid failed\n");
        free_guarded_kernel_stack(proc);
        return NULL;
    }
    lsm_task_alloc(proc->pid);
    proc->state = PROCESS_READY;
    proc->next = NULL;
    proc->pending_signals = 0;
    proc->sig_mask = 0;
    memset(proc->sig_handlers, 0, sizeof(proc->sig_handlers));
    memset(proc->sig_sa_mask, 0, sizeof(proc->sig_sa_mask));
    memset(proc->fd_table, 0, sizeof(proc->fd_table));
    proc->is_user = 1;
    proc->user_entry = entry;
    proc->user_rsp = user_rsp;
    proc->pml4 = pml4;
    kpti_setup_process(proc);
    proc->parent_pid = current_process ? current_process->pid : 0;
    proc->pgid = current_process ? current_process->pgid : proc->pid;
    proc->sid = current_process ? current_process->sid : proc->pid;
    proc->exit_code = 0;
    proc->sleep_until = 0;
    proc->is_background = 0;
    proc->is_suspended = 0;
    proc->priority = 1;
    proc->nice = NICE_DEFAULT;
    proc->cpu_affinity = 0;
    proc->base_priority = 1;
    proc->uid = 0;
    proc->gid = 0;
    proc->euid = 0;
    proc->egid = 0;
    proc->ngroups = 0;
    proc->umask = 0022;
    proc->wait_for_pid = 0;
    proc->ticks_remaining = 0;
    proc->last_run_tick = timer_get_ticks();
    process_set_cap_profile(proc, PROCESS_CAP_PROFILE_USER_DEFAULT);
    proc->sched_policy = SCHED_OTHER;
    proc->alt_stack_sp = NULL;
    proc->alt_stack_size = 0;
    proc->alt_stack_flags = SS_DISABLE;
    proc->personality = 0;
    proc->coredump_enabled = 1;
    proc->dumpable = 1; /* default: dumpable (SUID_DUMP_USER) */
    memset(proc->proc_comm, 0, sizeof(proc->proc_comm));
    strncpy(proc->proc_comm, name, sizeof(proc->proc_comm) - 1);
    proc->proc_comm[sizeof(proc->proc_comm) - 1] = '\0';
    proc->name = proc->proc_comm;
    memset(proc->exe_path, 0, sizeof(proc->exe_path));
    rlimit_init_defaults(proc);
    cap_bset_init(proc);
    proc->landlock_ruleset_ids[0] = -1; /* no landlock restrictions */
    proc->landlock_ruleset_ids[1] = -1;
    proc->landlock_ruleset_ids[2] = -1;
    proc->landlock_ruleset_ids[3] = -1;
    proc->ptracer_pid = 0; /* YAMA: no tracer allowed by default */
    kcov_process_init(proc);

    /* Inherit parent's bounding set */
    if (current_process && current_process->state != PROCESS_UNUSED) {
        for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++)
            proc->cap_bset[i] = current_process->cap_bset[i];
    }

    /* Initialize CPU time accounting */
    proc->utime_ticks = 0;
    proc->stime_ticks = 0;
    proc->start_time_tick = timer_get_ticks();
    proc->cpu_limit_warned_tick = 0;
    proc->nvcsw = 0;
    proc->nivcsw = 0;
    proc->minflt = 0;
    proc->majflt = 0;
    proc->stack_canary = prng_rand64();

    /* Set up initial context on kernel stack.
     * context_switch will pop r15..rbp then ret → user_entry_trampoline
     * which does iretq to ring 3.
     * r15 = user RIP, r14 = user RSP */
    uint64_t *sp = (uint64_t *)(proc->stack_top);
    sp -= 7;
    sp[0] = entry;                           /* r15 = user entry point */
    sp[1] = user_rsp;                        /* r14 = user stack pointer */
    sp[2] = 0;                               /* r13 */
    sp[3] = 0;                               /* r12 */
    sp[4] = 0;                               /* rbx */
    sp[5] = 0;                               /* rbp */
    sp[6] = (uint64_t)user_entry_trampoline; /* rip = ring3 trampoline */

    proc->context = (struct cpu_context *)sp;

    if (scheduler_add(proc) < 0) {
        /* Release the PID allocated above (was leaked before) */
        free_pid(proc->pid);
        free_guarded_kernel_stack(proc);
        proc->state = PROCESS_UNUSED;
        return NULL;
    }
    return proc;
}

/* Wake any process blocked in waitpid waiting for 'pid'. */
void process_wake_waiter(uint32_t pid) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &process_table[i];
        if (p->state == PROCESS_BLOCKED && p->wait_for_pid == pid) {
            p->wait_for_pid = 0;
            p->state = PROCESS_READY;
            p->last_run_tick = timer_get_ticks();
            scheduler_wakeup(p);
        }
    }
}

void process_exit(void) {
    /* Send SIGCHLD to parent with full siginfo_t */
    signal_notify_parent(current_process, CLD_EXITED, 0);
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_code = 0;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);
    scheduler_yield();
    /* should never reach here */
    for (;;)
        __asm__ volatile("hlt");
}

/* ── Orphaned process group detection ────────────────────────────────── */
/* Check if a process group in a given session is orphaned.
 * A process group is orphaned if for every member, the parent is either
 * in a different session or in a different process group (same session).
 * Returns 1 if orphaned, 0 if not. */
static int is_pgid_orphaned(uint32_t pgid, uint32_t sid) {
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED || table[i].state == PROCESS_ZOMBIE)
            continue;
        if (table[i].pgid != pgid || table[i].sid != sid)
            continue;
        /* Found a living member — check its parent */
        struct process *parent = process_get_by_pid(table[i].parent_pid);
        if (!parent || parent->state == PROCESS_UNUSED || parent->state == PROCESS_ZOMBIE)
            continue; /* parent gone → orphaned condition met for this member */
        /* If parent is in same session AND same process group, NOT orphaned */
        if (parent->sid == sid && parent->pgid == pgid)
            return 0;
    }
    return 1; /* orphaned */
}

/* Check for orphaned process groups in the given session.
 * For each orphaned process group, deliver SIGHUP followed by SIGCONT
 * to all members (POSIX job-control requirement). */
static void check_orphaned_process_groups(uint32_t sid) {
    struct process *table = process_get_table();
    uint32_t seen_pgids[PROCESS_MAX];
    int n_seen = 0;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED || table[i].state == PROCESS_ZOMBIE)
            continue;
        if (table[i].sid != sid)
            continue;

        uint32_t pgid = table[i].pgid;
        if (pgid == 0)
            continue;

        /* Skip duplicate pgids */
        int already_seen = 0;
        for (int j = 0; j < n_seen; j++) {
            if (seen_pgids[j] == pgid) {
                already_seen = 1;
                break;
            }
        }
        if (already_seen)
            continue;
        if (n_seen < PROCESS_MAX)
            seen_pgids[n_seen++] = pgid;

        if (is_pgid_orphaned(pgid, sid)) {
            /* POSIX: deliver SIGHUP then SIGCONT to orphaned process group */
            signal_send_group(pgid, SIGHUP);
            signal_send_group(pgid, SIGCONT);
        }
    }
}

void process_exit_code(int code) {
    /* Reparent orphans to init (PID 1) */
    uint32_t my_pid = current_process->pid;
    uint32_t my_sid = current_process->sid;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_UNUSED && process_table[i].parent_pid == my_pid &&
            process_table[i].pid != my_pid) {
            process_table[i].parent_pid = 1;
        }
    }
    /* Check for orphaned process groups in our session now that some
     * processes' parents have been reparented to init (PID 1), which
     * is typically in a different session. */
    check_orphaned_process_groups(my_sid);
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_code = code;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);

    /* CLONE_CHILD_CLEARTID: write 0 to userspace CTID pointer and futex-wake */
    if (current_process->ctid_ptr && current_process->is_user) {
        uint32_t zero = 0;
        if (copy_to_user((uint64_t)current_process->ctid_ptr, &zero, sizeof(zero)) < 0) {
            kprintf("[process] warning: failed to clear CTID at 0x%lx\n",
                    (unsigned long)current_process->ctid_ptr);
        }

        /* Wake up to 1 futex waiter on the CTID address (thread join) */
        uint32_t *ctid_uaddr = (uint32_t *)current_process->ctid_ptr;
        __asm__ volatile("cli");
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (futex_waiters[i].proc && futex_waiters[i].uaddr == ctid_uaddr) {
                struct process *wake_p = futex_waiters[i].proc;
                futex_waiters[i].proc = NULL;
                futex_waiters[i].uaddr = NULL;
                futex_num_waiters--;
                if (wake_p->state == PROCESS_BLOCKED) {
                    wake_p->state = PROCESS_READY;
                    scheduler_add(wake_p);
                }
                break; /* wake only 1 waiter */
            }
        }
        __asm__ volatile("sti");
    }
    /* Send SIGCHLD to parent with full siginfo_t */
    signal_notify_parent(current_process, CLD_EXITED, code);
    scheduler_yield();
    for (;;)
        __asm__ volatile("hlt");
}

/* ── O_CLOEXEC support ───────────────────────────────────────────────── */

/* Close all file descriptors with FD_CLOEXEC flag set */
void process_exec_close_cloexec(void) {
    struct process *cur = process_get_current();
    if (!cur)
        return;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cur->fd_table_lock, &irq_flags);
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (cur->fd_table[i].used && (cur->fd_table[i].flags & FD_CLOEXEC)) {
            cur->fd_table[i].used = 0;
            cur->fd_table[i].offset = 0;
            cur->fd_table[i].path[0] = '\0';
            cur->fd_table[i].flags = 0;
        }
    }
    spinlock_irqsave_release(&cur->fd_table_lock, irq_flags);
}

struct process *process_get_current(void) {
    struct process *proc = get_current_process();
    if (!proc)
        return current_process;
    return proc;
}

/* ── Capability bounding set ─────────────────────────────────────────── */

void cap_bset_drop(uint32_t cap) {
    struct process *p = process_get_current();
    if (!p || cap >= PROCESS_SYSCALL_MAX)
        return;
    int word = cap / 64;
    int bit = cap % 64;
    p->cap_bset[word] &= ~(1ULL << bit);
}

int cap_bset_has(uint32_t cap) {
    struct process *p = process_get_current();
    if (!p || cap >= PROCESS_SYSCALL_MAX)
        return 0;
    int word = cap / 64;
    int bit = cap % 64;
    return (p->cap_bset[word] & (1ULL << bit)) != 0;
}

void cap_bset_init(struct process *proc) {
    if (!proc)
        return;
    /* Initialize bounding set to all ones (all caps allowed by default) */
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++)
        proc->cap_bset[i] = ~0ULL;
}

/* ── Securebits ─────────────────────────────────────────────────────── */

int securebits_get(struct process *proc) {
    if (!proc)
        return 0;
    return (int)proc->securebits;
}

int securebits_set(struct process *proc, uint8_t bits) {
    if (!proc)
        return -EINVAL;
    /* Only allow setting bits that aren't locked */
    if (proc->securebits & SECBIT_LOCKED_MASK)
        return -EINVAL;
    /* Can only set bits that are in the allowed or locked mask */
    if (bits & ~(SECBIT_ALLOWED_MASK | SECBIT_LOCKED_MASK))
        return -EINVAL;
    /* Setting a locked bit requires the corresponding non-locked bit */
    if ((bits & SECBIT_KEEP_CAPS_LOCKED) && !(bits & SECBIT_KEEP_CAPS))
        return -EINVAL;
    if ((bits & SECBIT_NO_SETUID_FIXUP_LOCKED) && !(bits & SECBIT_NO_SETUID_FIXUP))
        return -EINVAL;
    if ((bits & SECBIT_NOROOT_LOCKED) && !(bits & SECBIT_NOROOT))
        return -EINVAL;
    proc->securebits = bits;
    return 0;
}

/* ── Capabilities on exec ────────────────────────────────────────────── */

void process_exec_caps(void) {
    struct process *p = process_get_current();
    if (!p)
        return;

    /* NOTE: the syscall_caps bitmap is the syscall-dispatch gate for this
     * kernel, NOT the Linux "permitted set".  Clearing it on exec would
     * leave every exec'd binary with zero allowed syscalls (exit itself
     * denied), so we preserve it across exec.  The per-process bounding
     * set is still ANDed below so caps can only drop, never grow. */

    /* Apply system-wide bounding set: per-process caps must be
     * further limited by the global bounding set that an admin
     * has configured via caps.c APIs (e.g., sys_cap_bset_drop). */
    sys_cap_bset_apply(p);

    /* bounding set &= permitted (caps can only be dropped, never gained) */
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++) {
        p->cap_bset[i] &= p->syscall_caps[i];
    }
}

/* ── Process credential API ─────────────────────────────────── */

int process_get_cred(uint32_t pid, uint32_t *uid, uint32_t *gid, uint32_t *euid, uint32_t *egid) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED)
        return -EINVAL;
    if (uid)
        *uid = p->uid;
    if (gid)
        *gid = p->gid;
    if (euid)
        *euid = p->euid;
    if (egid)
        *egid = p->egid;
    return 0;
}

int process_set_cred(uint32_t pid, uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED)
        return -EINVAL;
    p->uid = uid;
    p->gid = gid;
    p->euid = euid;
    p->egid = egid;
    return 0;
}

/* ── Dumpable flag ──────────────────────────────────────────────── */

int process_get_dumpable(struct process *proc) {
    if (!proc)
        return -EINVAL;
    return proc->dumpable;
}

int process_set_dumpable(struct process *proc, int val) {
    if (!proc)
        return -EINVAL;
    if (val != 0 && val != 1)
        return -EINVAL;
    proc->dumpable = val;
    return 0;
}

/* ── Exec credential security ──────────────────────────────────── */

void process_exec_cred_security(uint32_t orig_euid, uint32_t orig_egid) {
    struct process *p = process_get_current();
    if (!p)
        return;

    /* NO_NEW_PRIVS enforcement:
     * When no_new_privs is set, execve() must not gain new privileges.
     * We enforce this by blocking any setuid/setgid elevation, file
     * capability grants, and clearing the effective capability set
     * so the new binary starts with minimal privileges. */
    if (p->no_new_privs) {
        /* ── Block setuid/setgid elevation ─────────────────────── */
        /* If the new binary has setuid/setgid bits, keep the
         * existing euid/egid unchanged. NO_NEW_PRIVS means the
         * process never gains new privileges, ever. */
        /* (Setuid/setgid may have been applied by the caller; the
         *  caller is responsible for restoring euid after this.) */

        /* ── Block file capability elevation ───────────────────── */
        /* Clear ALL capability sets — the new binary starts with
         * zero capabilities regardless of file-attached caps. */
        process_caps_clear_all(p);
        memset(p->cap_effective, 0, sizeof(p->cap_effective));
        memset(p->cap_permitted, 0, sizeof(p->cap_permitted));
        memset(p->cap_inheritable, 0, sizeof(p->cap_inheritable));
        memset(p->cap_bset, 0, sizeof(p->cap_bset));

        /* ── Disable core dumps (sensitive exec) ──────────────── */
        p->dumpable = 0; /* SUID_DUMP_DISABLE */

        /* ── Clear securebits for fresh start with no privs ───── */
        p->securebits = 0;

        return;
    }

    /* ── SECBIT_NOROOT enforcement ────────────────────────────── */
    /* When SECBIT_NOROOT is set, the process must not gain root
     * privileges even if the new binary would normally grant them.
     * Force euid/egid to the real uid/gid if root would be gained. */
    if (p->securebits & SECBIT_NOROOT) {
        /* If the new binary is setuid-root, keep uid as real uid */
        if (p->euid != p->uid && p->euid == 0 && p->uid != 0) {
            p->euid = p->uid;
        }
        if (p->egid != p->gid && p->egid == 0 && p->gid != 0) {
            p->egid = p->gid;
        }
    }

    /* ── Apply capabilities on exec ───────────────────────────── */
    /*  - If SECBIT_KEEP_CAPS is not set, clear the permitted set
     *  - AND bounding set with permitted set (caps drop but never gain) */
    process_exec_caps();

    /* ── Detect credential change ─────────────────────────────── */
    /* Compare current credentials against the originals saved by the
     * caller (before setuid/setgid was applied) to detect changes.
     * If euid or egid changed during exec (e.g., setuid binary),
     * we reduce privileges. */
    int creds_changed = (p->euid != orig_euid || p->egid != orig_egid);

    if (creds_changed) {
        /*
         * When credentials change:
         *  - Disable core dumps (SUID_DUMP_DISABLE)
         *  - Clear securebits exec state
         *  - Reset capability effective set
         */
        p->dumpable = 0; /* SUID_DUMP_DISABLE */

        /* Clear sensitive state: if the new binary changed credentials,
         * we must not leak privileged state from before exec. */
        /* Clear securebits keep_caps (re-evaluate on exec) */
        p->securebits &= ~(uint8_t)SECBIT_KEEP_CAPS;
    }
    /* else: no credential change — dumpable stays as-is (default 1) */
}

/*
 * fork() — clone current process, child starts at fork_child_entry.
 * Returns child PID to parent, -1 on error.
 * NOTE: The child does NOT return from process_fork() with value 0.
 * Instead, the child begins execution in fork_child_entry().
 */
int process_fork(void);
extern void fork_child_trampoline(void);
extern uint64_t syscall_user_rsp;
extern uint64_t syscall_user_rip;
extern uint64_t syscall_user_rflags;
extern uint64_t syscall_user_r15;
extern uint64_t syscall_user_r14;
extern uint64_t syscall_user_r13;
extern uint64_t syscall_user_r12;
extern uint64_t syscall_user_rbx;
extern uint64_t syscall_user_rbp;

int process_fork(void) {
    struct process *parent = current_process;
    struct process *child = NULL;

    /* ── Snapshot the user registers captured at syscall entry ─────
     * These live in GLOBAL variables (syscall_user_r*). Reading them
     * lazily at the bottom of this function is racy: alloc_guarded_
     * kernel_stack() / vmm_clone_user_pml4() / kpti_setup_process()
     * can take long enough for a timer tick to fire, the scheduler
     * then runs another task (e.g. netd), and ITS syscalls overwrite
     * the globals — so the child would inherit netd's (or worse,
     * garbage) registers and execve with a corrupt path/argv (seen:
     * "execve(path=0x5) failed: -EFAULT" + boot hang). Snapshot once
     * here, before any preemption point. */
    uint64_t fork_ursp = syscall_user_rsp;
    uint64_t fork_urip = syscall_user_rip;
    uint64_t fork_urfl = syscall_user_rflags;
    uint64_t fork_ur15 = syscall_user_r15;
    uint64_t fork_ur14 = syscall_user_r14;
    uint64_t fork_ur13 = syscall_user_r13;
    uint64_t fork_ur12 = syscall_user_r12;
    uint64_t fork_urbx = syscall_user_rbx;
    uint64_t fork_urbp = syscall_user_rbp;

    /* RLIMIT_NPROC: count processes owned by the same UID */
    uint64_t nproc_limit = parent->rlim_cur[RLIMIT_NPROC];
    if (nproc_limit != ~0ULL) {
        uint64_t same_user_count = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (process_table[i].state != PROCESS_UNUSED &&
                process_table[i].state != PROCESS_ZOMBIE && process_table[i].uid == parent->uid) {
                same_user_count++;
            }
        }
        if (same_user_count >= nproc_limit) {
            return -EAGAIN;
        }
    }

    __asm__ volatile("cli");

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            child = &process_table[i];
            break;
        }
    }
    if (unlikely(!child)) {
        __asm__ volatile("sti");
        return -EAGAIN;
    }

    child->state = PROCESS_UNUSED;
    *child = *parent;
    spinlock_init(&child->fd_table_lock);
    /* After fork, parent and child share file offsets.
     * For each in-use fd, allocate a shared uint64_t with the
     * current offset so both parent and child see the same
     * file position. */
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (parent->fd_table[i].used) {
            uint64_t *shared = kmalloc(sizeof(uint64_t));
            if (shared) {
                *shared = parent->fd_table[i].offset;
                parent->fd_table[i].shared_offset = shared;
                child->fd_table[i].shared_offset = shared;
                child->fd_table[i].offset = *shared; /* keep local copy in sync */
            }
        }
    }
    child->pid = alloc_pid();
    if (child->pid == (uint32_t)-1) {
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -EAGAIN;
    }
    child->parent_pid = parent->pid;
    child->is_suspended = 0;
    child->stack_canary = prng_rand64();
    lsm_task_alloc(child->pid);

    if (alloc_guarded_kernel_stack(child) < 0) {
        free_pid(child->pid);
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -ENOMEM;
    }
    child->state = PROCESS_READY;

    sys_cap_bset_apply(child);

    if (parent->sched_flags & SCHED_FLAG_RESET_ON_FORK) {
        memset(child->cap_effective, 0, sizeof(child->cap_effective));
    }

    if (parent->pml4) {
        child->pml4 = vmm_clone_user_pml4(parent->pml4);
        if (!child->pml4) {
            free_pid(child->pid);
            free_guarded_kernel_stack(child);
            child->state = PROCESS_UNUSED;
            __asm__ volatile("sti");
            return -EROFS;
        }
        vmm_switch_pml4(parent->pml4);
        kpti_setup_process(child);
    }

    /* ── Validate user stack pointer ─────────────
     * RSP must be a user address, 8-byte aligned.  NOTE: at a syscall
     * entry RSP is 8 mod 16 (the userspace `call` that invoked the libc
     * wrapper pushed a return address), so the old 16-byte alignment
     * check wrongly rejected every fork() and the child was never
     * scheduled. */
    if (!fork_ursp || fork_ursp >= USER_VADDR_MAX || (fork_ursp & 0x7) != 0) {
        free_pid(child->pid);
        free_guarded_kernel_stack(child);
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -EFAULT;
    }

    /* Set up child's kernel stack so that when context_switch() restores
     * it, execution flows: pop r15..rbp (6 slots) → ret → fork_child_trampoline,
     * whose RSP then points at [user RFLAGS][user RIP][user RSP] (see
     * switch.asm fork_child_trampoline).  Layout (from stack_top down): */
    uint64_t *sp = (uint64_t *)child->stack_top;
    sp -= 10;
    /* The child must inherit the parent's user callee-saved registers
     * (r15..rbp): userspace code legitimately keeps values in them across
     * the fork() call (e.g. init holds the getty argv/envp pointers in
     * r13/rbx).  Zeroing them made the child see NULL argv/envp — getty
     * ran with argc=0 and respawned forever.  The values are captured by
     * the syscall entry (syscall_user_r* globals). */
    sp[0] = fork_ur15;                       /* r15 (popped by context_switch) */
    sp[1] = fork_ur14;                       /* r14 */
    sp[2] = fork_ur13;                       /* r13 */
    sp[3] = fork_ur12;                       /* r12 */
    sp[4] = fork_urbx;                       /* rbx */
    sp[5] = fork_urbp;                       /* rbp */
    sp[6] = (uint64_t)fork_child_trampoline; /* ret target → trampoline */
    sp[7] = fork_urfl;                       /* popped into r11 for sysret */
    sp[8] = fork_urip;                       /* popped into rcx for sysret */
    sp[9] = fork_ursp;                       /* popped into rsp for sysret */
    child->context = (struct cpu_context *)sp;

    if (scheduler_add(child) < 0) {
        /* Destroy the freshly cloned user page tables (fork deep-copies) */
        if (child->pml4)
            vmm_destroy_user_pml4(child->pml4);
        free_pid(child->pid);
        free_guarded_kernel_stack(child);
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -EINVAL;
    }
    /* Child-runs-first boost: give the fresh child the same EEVDF
     * eligible-deadline 0 treatment as the netd's wake-boost, so it
     * wins the next pick after the parent blocks.  Without this, the
     * child (deadline = now + slice) loses every pick to the netd's
     * permanent deadline=0 boost and is starved indefinitely (observed:
     * boot hangs with the forked child never running after the parent's
     * first COW fault).  sched_boost_on_wake keeps the boost applied on
     * EVERY re-enqueue — a one-shot deadline=0 is lost when the child
     * is preempted and re-added (eevdf_enqueue resets the deadline). */
    child->eevdf_deadline = 0;
    child->sched_boost_on_wake = 1;
    __asm__ volatile("sti");
    return (int)child->pid;
}

/* ── Clone: create a thread (child may share address space) ──── */
extern void clone_child_trampoline(void);

/* Roll back namespace setup performed by process_clone's CLONE_NEW*
 * handling.  Must be called on every error return AFTER the namespace
 * block has run (i.e. after child->pid_ns/cgroup_ns/mnt_ns/user_ns have
 * been set up): it releases both freshly-created namespaces (CLONE_NEW*)
 * and the inherited-reference increments (cgroup_ns_get/mnt_ns_get) so a
 * failed clone leaks neither namespace slots nor refcounts. */
/* Roll back the PID-namespace state set up by process_clone's NEWPID
 * handling: either a fresh namespace created for this child (CLONE_NEWPID)
 * or a local PID allocated in an inherited non-root namespace.  Safe to
 * call on any error path once the NEWPID block has run. */
static void clone_rollback_pidns(struct process *child, struct process *parent) {
    if (child->pid_ns && child->pid_ns != parent->pid_ns) {
        /* CLONE_NEWPID: namespace was created for this child */
        pid_ns_destroy(child->pid_ns);
    } else if (child->pid_ns && child->pid_ns != &init_pid_ns && child->ns_pid > 0) {
        /* Inherited non-root namespace: release the local pid we allocated */
        pid_ns_free_pid(child->pid_ns, child->ns_pid);
    }
}

static void clone_rollback_namespaces(struct process *child, struct process *parent,
                                      bool user_ns_ref_taken) {
    clone_rollback_pidns(child, parent);
    if (child->cgroup_ns) {
        cgroup_ns_put(child->cgroup_ns);
        child->cgroup_ns = NULL;
    }
    if (child->mnt_ns) {
        mnt_ns_put(child->mnt_ns);
        child->mnt_ns = NULL;
    }
    /* Release the user-namespace reference: either a fresh CLONE_NEWUSER
     * namespace (creation refcount 1) or an inherited reference taken by
     * user_ns_get() in the else branch.  user_ns_ref_taken distinguishes
     * the latter from the plain struct-copy alias of the parent's
     * namespace, which holds no reference and must not be put. */
    if (child->user_ns && (child->user_ns != parent->user_ns || user_ns_ref_taken)) {
        user_ns_put(child->user_ns);
        child->user_ns = NULL;
    }
}

int process_clone(struct process *parent, uint64_t flags, void *child_stack, uint64_t user_rip,
                  uint64_t user_rflags) {
    struct process *child = NULL;

    /* Snapshot the syscall-entry user registers once, up front.  Reading
     * the syscall_user_r* globals later (after allocations/preemption)
     * races with other processes' syscalls clobbering them (see the
     * process_fork snapshot comment). */
    uint64_t clone_ursp = syscall_user_rsp;

    /* Set when the CLONE_NEWUSER else-branch takes an inherited
     * user-namespace reference; lets clone_rollback_namespaces() release
     * it on error without touching the plain struct-copy alias. */
    bool user_ns_ref_taken = false;

    /* ── Validate user-space stack pointer ───────────────────── */
    /* For user-mode callers, ensure child_stack is a valid user address.
     * If child_stack is NULL, inherit the parent's stack pointer (fork semantics).
     * For kernel-mode callers, child_stack may be a function pointer — skip
     * validation. */
    if (parent->is_user) {
        uint64_t stack_addr = (uint64_t)(uintptr_t)child_stack;
        if (stack_addr == 0) {
            /* NULL child_stack: inherit parent's stack pointer */
            if (!clone_ursp || clone_ursp >= USER_VADDR_MAX || (clone_ursp & 0xF) != 0)
                return -EFAULT;
            child_stack = (void *)clone_ursp;
        } else {
            /* Explicit stack pointer: validate it */
            if (stack_addr >= USER_VADDR_MAX || (stack_addr & 0xF) != 0)
                return -EINVAL;
        }
    }

    /* RLIMIT_NPROC: count processes owned by the same UID */
    uint64_t nproc_limit = parent->rlim_cur[RLIMIT_NPROC];
    if (nproc_limit != ~0ULL) {
        uint64_t same_user_count = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (process_table[i].state != PROCESS_UNUSED &&
                process_table[i].state != PROCESS_ZOMBIE && process_table[i].uid == parent->uid) {
                same_user_count++;
            }
        }
        if (same_user_count >= nproc_limit) {
            return -EAGAIN;
        }
    }

    __asm__ volatile("cli");

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            child = &process_table[i];
            break;
        }
    }
    if (unlikely(!child)) {
        __asm__ volatile("sti");
        return -EINVAL;
    }

    child->state = PROCESS_UNUSED;
    *child = *parent;
    child->pid = alloc_pid();
    lsm_task_alloc(child->pid);
    /* After fork/clone, parent and child share file offsets */
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (parent->fd_table[i].used) {
            uint64_t *shared = kmalloc(sizeof(uint64_t));
            if (shared) {
                *shared = parent->fd_table[i].offset;
                parent->fd_table[i].shared_offset = shared;
                child->fd_table[i].shared_offset = shared;
                child->fd_table[i].offset = *shared;
            }
        }
    }
    if (child->pid == (uint32_t)-1) {
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -EINVAL;
    }
    child->parent_pid = parent->pid;
    child->is_suspended = 0;
    child->wait_for_pid = 0;
    child->on_queue = 0;
    child->context = NULL;
    child->next = NULL;
    child->tgid = (flags & CLONE_THREAD) ? parent->tgid : child->pid;

    /* Per POSIX / Linux semantics: after fork/clone, the child inherits
     * NO pending signals from the parent.  The pending signal set must
     * be cleared, along with the per-signal siginfo storage, so the
     * child does not see signals that were destined for the parent. */
    child->pending_signals = 0;
    memset(child->sig_info, 0, sizeof(child->sig_info));

    /* ── Handle CLONE_NEWPID: child gets a new PID namespace (Item 111) ── */
    if (flags & CLONE_NEWPID) {
        struct pid_namespace *new_ns = pid_ns_create(parent->pid_ns);
        if (unlikely(!new_ns)) {
            free_pid(child->pid);
            child->state = PROCESS_UNUSED;
            __asm__ volatile("sti");
            return -EINVAL;
        }
        child->pid_ns = new_ns;
        child->ns_pid = pid_ns_alloc_pid(new_ns);
        if (child->ns_pid == 0) {
            /* PID 0 is reserved; the first allocatable PID is 1 (init) */
            child->ns_pid = pid_ns_alloc_pid(new_ns);
        }
        kprintf("[PIDNS] clone(NEWPID): child pid=%d is init (ns_pid=%d) in new namespace id=%d\n",
                child->pid, child->ns_pid, new_ns->id);
    } else {
        /* Inherit parent's PID namespace */
        child->pid_ns = parent->pid_ns;
        if (child->pid_ns && child->pid_ns != &init_pid_ns) {
            /* Non-root namespace: allocate a local PID */
            child->ns_pid = pid_ns_alloc_pid(child->pid_ns);
        } else {
            child->ns_pid = child->pid; /* root ns: ns_pid == global pid */
        }
    }

    /* ── Handle CLONE_NEWCGROUP: child gets a new cgroup namespace (Item 117) ── */
    if (flags & CLONE_NEWCGROUP) {
        /* Use the parent's cgroup path as the namespace root */
        struct cgroup_namespace *new_ns =
            cgroup_ns_create(parent->cgroup_ns ? parent->cgroup_ns->root_path : "/");
        if (unlikely(!new_ns)) {
            /* The NEWPID block above may have created a fresh namespace or
             * taken a local PID in an inherited one — roll that back.  The
             * cgroup/mnt/user blocks have not run yet, so no other
             * namespace references are held here. */
            clone_rollback_pidns(child, parent);
            free_pid(child->pid);
            child->state = PROCESS_UNUSED;
            __asm__ volatile("sti");
            return -EINVAL;
        }
        /* The struct copy above only aliased the parent's namespace: no
         * reference was taken on it (the child's inherited reference is
         * taken by cgroup_ns_get() in the else branch below).  Putting it
         * here would release a ref the child never held and could free a
         * namespace the parent still uses.  The new namespace's creation
         * refcount (1) is the child's membership reference instead. */
        child->cgroup_ns = new_ns;
        kprintf("[CGROUP_NS] clone(NEWCGROUP): child pid=%d, root='%s'\n", child->pid,
                new_ns->root_path);
    } else if (parent->cgroup_ns) {
        /* Increment refcount on inherited namespace */
        cgroup_ns_get(child->cgroup_ns);
    }

    /* ── Handle CLONE_NEWNS: child gets a new mount namespace (Item 112) ── */
    if (flags & CLONE_NEWNS) {
        struct mnt_namespace *new_ns = mnt_ns_copy(parent->mnt_ns ? parent->mnt_ns : NULL);
        if (unlikely(!new_ns)) {
            /* Roll back the PID-namespace state AND the cgroup-namespace
             * reference taken by the block above (a fresh CLONE_NEWCGROUP
             * namespace or the inherited cgroup_ns_get).  The mount-namespace
             * reference is only taken in the else branch, which has not run
             * yet, so no mnt ref is held here. */
            clone_rollback_pidns(child, parent);
            if (child->cgroup_ns) {
                cgroup_ns_put(child->cgroup_ns);
                child->cgroup_ns = NULL;
            }
            free_pid(child->pid);
            child->state = PROCESS_UNUSED;
            __asm__ volatile("sti");
            return -EINVAL;
        }
        /* Same as NEWCGROUP: the struct copy took no reference on the
         * parent's mount namespace, so there is nothing to release here.
         * The new namespace's creation refcount (1) is the child's
         * membership reference. */
        child->mnt_ns = new_ns;
        kprintf("[MNT_NS] clone(NEWNS): child pid=%d\n", child->pid);
    } else if (child->mnt_ns) {
        /* Increment refcount on inherited mount namespace */
        mnt_ns_get(child->mnt_ns);
    }

    /* ── Handle CLONE_NEWUSER: child gets a new user namespace (Item 114) ── */
    if (flags & CLONE_NEWUSER) {
        struct user_namespace *new_ns = user_ns_create(
            parent->user_ns ? parent->user_ns : &init_user_ns, parent->uid, parent->gid);
        if (unlikely(!new_ns)) {
            /* All namespace blocks have now run: release the PID-namespace
             * state plus the cgroup and mount references (and the fresh
             * user namespace, had it been created — it was not). */
            clone_rollback_namespaces(child, parent, user_ns_ref_taken);
            free_pid(child->pid);
            child->state = PROCESS_UNUSED;
            __asm__ volatile("sti");
            return -EINVAL;
        }
        child->user_ns = new_ns;
        /* Inside the new user namespace, the child is initially mapped
         * to UID 0 (root).  The parent's UID/GID in the parent namespace
         * are mapped to 0 inside — set the child's euid to 0 to reflect
         * that it has root-equivalent privileges inside this namespace. */
        child->uid = 0;
        child->gid = 0;
        child->euid = 0;
        child->egid = 0;
        kprintf("[USERNS] clone(NEWUSER): child pid=%d is root in new namespace id=%d\n",
                child->pid, new_ns->id);
    } else {
        /* Inherit parent's user namespace (take a reference) */
        child->user_ns = user_ns_get(parent->user_ns);
        user_ns_ref_taken = true;
    }

    /* Allocate fresh kernel stack */
    if (alloc_guarded_kernel_stack(child) < 0) {
        /* Roll back namespaces allocated/ref'd by the CLONE_NEW* block
         * AND release the PID allocated above (was leaked before). */
        clone_rollback_namespaces(child, parent, user_ns_ref_taken);
        free_pid(child->pid);
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -EINVAL;
    }

    /* Handle CLONE_VM — share address space */
    if (flags & CLONE_VM) {
        /* Child shares parent's page tables — no copy needed */
        /* Kernel page tables are already shared via the higher-half mapping */
        if (parent->pml4) {
            child->pml4 = parent->pml4;
        }
    } else {
        /* Full fork-style: deep-copy user address space */
        if (parent->pml4) {
            child->pml4 = vmm_clone_user_pml4(parent->pml4);
            if (!child->pml4) {
                /* Roll back the CLONE_NEW* namespaces (were leaked before) */
                clone_rollback_namespaces(child, parent, user_ns_ref_taken);
                free_pid(child->pid);
                free_guarded_kernel_stack(child);
                child->state = PROCESS_UNUSED;
                __asm__ volatile("sti");
                return -EINVAL;
            }
            vmm_switch_pml4(parent->pml4);
        }
    }

    /* Handle CLONE_FILES — FD table is already inherited by the struct copy above.
     * With CLONE_FILES: child shares parent's FD table (shallow copy from struct copy).
     * Without CLONE_FILES: child gets its own private copy (deep copy from struct copy). */

    child->state = PROCESS_READY;

    /* Apply system-wide bounding set on clone — caps must respect
     * the global mask even if the parent had different caps. */
    sys_cap_bset_apply(child);

    /* SCHED_FLAG_RESET_ON_FORK: if parent had this scheduling flag
     * set, the child's effective capability set must be cleared so
     * that elevated capabilities aren't accidentally inherited by
     * unprivileged children (Linux-compatible security behavior). */
    if (parent->sched_flags & SCHED_FLAG_RESET_ON_FORK) {
        memset(child->cap_effective, 0, sizeof(child->cap_effective));
    }

    /* Set up child's kernel stack with sysret return frame.
     * Layout (from stack_top down):
     *   [context_switch frame: r15..rbp, rip → clone_child_trampoline]
     *   [syscall return frame: r15..rbp, r11, rcx, user_rsp]
     */
    uint64_t *sp = (uint64_t *)child->stack_top;

    /* Syscall return frame (9 values, bottom): */
    sp -= 9;
    sp[0] = 0;                     /* junk r15 (unused) */
    sp[1] = 0;                     /* junk r14 */
    sp[2] = 0;                     /* junk r13 */
    sp[3] = 0;                     /* junk r12 */
    sp[4] = 0;                     /* junk rbx */
    sp[5] = 0;                     /* junk rbp */
    sp[6] = user_rflags;           /* r11 → user RFLAGS for sysret */
    sp[7] = user_rip;              /* rcx → user RIP for sysret */
    sp[8] = (uint64_t)child_stack; /* user RSP for sysret */

    /* Context switch frame (7 values, above): */
    sp -= 7;
    sp[0] = 0;                                /* r15 */
    sp[1] = 0;                                /* r14 */
    sp[2] = 0;                                /* r13 */
    sp[3] = 0;                                /* r12 */
    sp[4] = 0;                                /* rbx */
    sp[5] = 0;                                /* rbp */
    sp[6] = (uint64_t)clone_child_trampoline; /* rip → trampoline */

    child->context = (struct cpu_context *)sp;

    if (scheduler_add(child) < 0) {
        /* Roll back namespaces + the freshly cloned pml4 (non-CLONE_VM) */
        clone_rollback_namespaces(child, parent, user_ns_ref_taken);
        if (!(flags & CLONE_VM) && child->pml4 && child->pml4 != parent->pml4)
            vmm_destroy_user_pml4(child->pml4);
        free_pid(child->pid);
        free_guarded_kernel_stack(child);
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -EINVAL;
    }
    __asm__ volatile("sti");
    return (int)child->pid;
}

void process_set_current(struct process *proc) {
    set_current_process(proc);
    current_process = proc;
}

struct process *process_get_by_pid(uint32_t pid) {
    /* Validate PID range: only 0..PROCESS_MAX-1 are valid table indices.
     * PIDs outside this range can never exist — reject early to avoid
     * a useless linear scan and to harden against callers passing
     * uninitialised or attacker-controlled values. */
    if (pid >= PROCESS_MAX)
        return NULL;

    struct process *fallback = NULL;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_UNUSED && process_table[i].pid == pid) {
            /* Prefer non-zombie processes (live) over zombie (dead) */
            if (process_table[i].state != PROCESS_ZOMBIE)
                return &process_table[i];
            if (!fallback)
                fallback = &process_table[i];
        }
    }
    return fallback;
}

/* Get a process by PID with Ring 3 visibility check.
 * Returns NULL if the caller process cannot see the target (permission denied).
 * Kernel-internal callers should use process_get_by_pid() directly. */
struct process *process_get_by_pid_visible(uint32_t pid) {
    struct process *target = process_get_by_pid(pid);
    if (!target)
        return NULL;
    struct process *cur = process_get_current();
    if (!cur)
        return target; /* kernel context, allow */
    if (!process_can_see(cur, target))
        return NULL;
    return target;
}

/* For shell `ps` command */
struct process *process_get_table(void) {
    return process_table;
}

/* Check if the caller process can see (access) the target process.
 * Returns 1 if visible, 0 if not. */
int process_can_see(const struct process *caller, const struct process *target) {
    if (!caller || !target)
        return 0;
    if (caller == target)
        return 1; /* self */

    /* PID namespace visibility check (Item 111):
     * A process can only see processes in its own namespace
     * or child namespaces (visible from parent namespace). */
    if (!pid_ns_visible(caller, target))
        return 0;

    if (caller->euid == 0)
        return 1; /* root sees all */
    if (caller->euid == target->euid)
        return 1; /* same uid */
    if (target->parent_pid == caller->pid)
        return 1; /* caller is parent */
    return 0;
}

/* Wait for a specific child process to become ZOMBIE.
 * Returns 0 on success (exit code in *status), -1 if not found.
 * If options & WNOHANG (1), returns 0 immediately if no child has exited
 * (does not block).  Otherwise blocks (does NOT spin) until the child
 * exits. */
int process_waitpid(uint32_t pid, int *status, int options) {
    struct process *child = process_get_by_pid(pid);
    if (!child)
        return -ENOENT;

    if (child->state != PROCESS_ZOMBIE && child->state != PROCESS_UNUSED) {
        /* WNOHANG: return 0 immediately instead of blocking */
        if (options & 1) /* WNOHANG */
            return 0;

        /* Block until child's process_exit_code wakes us.
         * Use process_get_current() (per-CPU) — NOT current_process (global).
         * The global 'current_process' is updated by all CPUs via
         * process_set_current() in schedule() and is racy on SMP.  Using
         * the per-CPU version guarantees we modify the correct process
         * even if another CPU has called schedule() since we last ran. */
        struct process *cur = process_get_current();
        if (!cur)
            return -EINVAL;
        cur->wait_for_pid = pid;
        cur->state = PROCESS_BLOCKED;
        scheduler_remove(cur);
        scheduler_yield();
        /* After wake, re-lookup the child — it may have been cleaned up
         * by another concurrent waitpid (race). */
        cur = process_get_current();
        cur->wait_for_pid = 0;
        child = process_get_by_pid(pid);
        if (!child || child->state == PROCESS_UNUSED)
            return -EINVAL;
    }

    if (status)
        *status = child->exit_code;
    process_cleanup(child);
    return 0;
}

/* Block the current process for N ticks.
 * Uses process_get_current() (per-CPU) — NOT current_process (global).
 * The global is racy on SMP because all CPUs write to it via
 * process_set_current() in schedule(). */
void process_sleep_ticks(uint64_t nticks) {
    struct process *cur = process_get_current();
    if (!cur)
        return;
    cur->sleep_until = timer_get_ticks() + nticks;
    cur->state = PROCESS_BLOCKED;
    scheduler_remove(cur);
    scheduler_yield();
}

/* Free resources of a zombie process. */
uint32_t process_get_count(void) {
    struct process *table = process_get_table();
    uint32_t count = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED)
            count++;
    }
    return count;
}

void process_cleanup(struct process *proc) {
    /* Idempotency guard: a slot already reaped must not be cleaned again.
     * All callers check state == PROCESS_ZOMBIE before calling, but that
     * check-then-act is not atomic — a concurrent reaper (another thread
     * in wait4, or the periodic process_reap_zombies) can observe ZOMBIE
     * while we are mid-cleanup.  Without this guard the second cleanup
     * would double-put the pid namespace (pid_ns_free_pid/pid_ns_destroy)
     * and re-walk the dead process's futex robust list. */
    if (proc->state == PROCESS_UNUSED)
        return;

    /* Claim the slot FIRST so any concurrent reaper that observed ZOMBIE
     * sees UNUSED before a single resource is released — closing the
     * window between the state check and the resource frees below. */
    proc->state = PROCESS_UNUSED;

    /* Cleanup robust futex list */
    futex_robust_list_cleanup(proc);
    proc->ctid_ptr = NULL;
    /* Cleanup KCOV coverage buffer (Item 208) */
    kcov_process_exit(proc);

    if (proc->kernel_stack) {
        free_guarded_kernel_stack(proc);
    }
    /* Free user page tables (fixes leak) */
    if (proc->is_user && proc->pml4) {
        vmm_destroy_user_pml4(proc->pml4);
        proc->pml4 = NULL;
    }
    if (proc->pid) {
        free_pid(proc->pid);
        lsm_task_free(proc->pid);
    }
    /* Free namespace-local PID (Item 111) */
    if (proc->pid_ns && proc->pid_ns != &init_pid_ns && proc->ns_pid > 0) {
        pid_ns_free_pid(proc->pid_ns, proc->ns_pid);
        /* If this was the last process in the namespace, destroy it */
        if (proc->pid_ns->process_count <= 0) {
            pid_ns_destroy(proc->pid_ns);
        }
        proc->pid_ns = NULL;
        proc->ns_pid = 0;
    }
    /* Release cgroup namespace reference (Item 117) */
    if (proc->cgroup_ns) {
        cgroup_ns_put(proc->cgroup_ns);
        proc->cgroup_ns = NULL;
    }
    /* Release mount namespace reference (Item 112) */
    if (proc->mnt_ns) {
        mnt_ns_put(proc->mnt_ns);
        proc->mnt_ns = NULL;
    }
    /* Release user namespace reference (Item 114) */
    if (proc->user_ns) {
        user_ns_put(proc->user_ns);
        proc->user_ns = NULL;
    }
    proc->pid = 0;
    proc->name = NULL;
    proc->context = NULL;
    proc->next = NULL;
    proc->sleep_until = 0;
    proc->is_background = 0;
    proc->is_suspended = 0;
    proc->pgid = 0;
    proc->sid = 0;
    proc->priority = 1;
    proc->nice = NICE_DEFAULT;
    proc->cap_profile = PROCESS_CAP_PROFILE_NONE;
    process_caps_clear_all(proc);
    memset(proc->fd_table, 0, sizeof(proc->fd_table));
}

/* Reap zombie processes: background jobs are reaped immediately,
 * other zombies are reaped only if their parent is gone.
 * Orphaned zombies reparented to init (PID 1) are always reaped —
 * init is trusted to collect all children and must not accumulate
 * zombies even if it is busy processing other events. */
void process_reap_zombies(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_ZOMBIE) {
            /* Background processes: reap immediately (no one waits for them) */
            if (process_table[i].is_background) {
                process_cleanup(&process_table[i]);
                continue;
            }
            /* Non-background: reap if parent is gone */
            struct process *parent = process_get_by_pid(process_table[i].parent_pid);
            if (!parent || parent->state == PROCESS_ZOMBIE || parent->state == PROCESS_UNUSED) {
                process_cleanup(&process_table[i]);
                continue;
            }
            /* Init (PID 1) auto-reaps orphaned zombies.
             * When a child has been reparented to init, it must be
             * cleaned up automatically without requiring init to call
             * waitpid().  In Linux this is handled via the child_reaper
             * check in release_task(). */
            if (process_table[i].parent_pid == 1) {
                process_cleanup(&process_table[i]);
            }
        }
    }
}

/* ── Per-CPU kthread API ────────────────────────────────────── */

struct process *kthread_create(void (*entry)(void *arg), void *arg, const char *name) {
    return kthread_create_on_cpu(entry, arg, name, -1);
}

struct process *kthread_create_on_cpu(void (*entry)(void *arg), void *arg, const char *name,
                                      int cpu_id) {
    struct process *proc = process_create((void (*)(void))entry, name);
    if (!proc)
        return NULL;

    proc->kthread_arg = arg;
    if (cpu_id >= 0 && cpu_id < SMP_MAX_CPUS)
        proc->cpu_affinity = (uint8_t)(1U << cpu_id);
    else
        proc->cpu_affinity = 0; /* any CPU */

    return proc;
}

/* ── Thread management for pthread support ────────────────────── */

/*
 * Thread info structure for pthread_create/pthread_join.
 * Each thread gets an entry in a fixed-size table. When the thread
 * finishes execution, the wrapper stores the return value and sets
 * the finished flag, then becomes a zombie. pthread_join reads the
 * stored return value and reclaims the thread info.
 */
#define THREAD_INFO_MAX 64

struct thread_info {
    volatile int used;
    volatile int finished;
    int pid;      /* thread's kernel PID */
    void *retval; /* returned value from start_routine */
};

static struct thread_info g_thread_info[THREAD_INFO_MAX];
static int g_thread_info_inited = 0;

/* Thread wrapper: calls the user's start routine, stores return value. */
struct thread_start_args {
    void *(*start_routine)(void *);
    void *arg;
    int info_idx; /* index into g_thread_info[] */
};

static void thread_wrapper(void *arg) {
    struct thread_start_args *tsa = (struct thread_start_args *)arg;
    void *(*start_routine)(void *) = tsa->start_routine;
    void *user_arg = tsa->arg;
    int idx = tsa->info_idx;

    /* Call the user's start routine */
    void *retval = start_routine(user_arg);

    /* Store return value and mark finished */
    if (idx >= 0 && idx < THREAD_INFO_MAX && g_thread_info[idx].used) {
        g_thread_info[idx].retval = retval;
        g_thread_info[idx].finished = 1;
    }

    /* Exit the thread — kernel will wake parent in waitpid */
    /* Use process_exit_code to exit */
    current_process->state = PROCESS_ZOMBIE;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);
    for (;;)
        __asm__ volatile("hlt"); /* should never reach here */
}

/*
 * Initialize thread info table (called once during boot).
 */
void thread_info_init(void) {
    if (g_thread_info_inited)
        return;
    memset(g_thread_info, 0, sizeof(g_thread_info));
    g_thread_info_inited = 1;
}

/*
 * Create a new thread that executes start_routine(arg).
 * Returns the thread ID (PID) on success, or -1 on error.
 */
int process_thread_create(void *(*start_routine)(void *), void *arg) {
    if (!start_routine)
        return -EINVAL;
    if (!g_thread_info_inited)
        thread_info_init();

    /* Find a free thread info slot */
    int idx = -1;
    for (int i = 0; i < THREAD_INFO_MAX; i++) {
        if (!g_thread_info[i].used) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -EINVAL;

    /* Allocate and fill the start arguments */
    struct thread_start_args *tsa =
        (struct thread_start_args *)kmalloc(sizeof(struct thread_start_args));
    if (unlikely(!tsa))
        return -ENOMEM;
    tsa->start_routine = start_routine;
    tsa->arg = arg;
    tsa->info_idx = idx;

    /* Prepare the thread info slot */
    g_thread_info[idx].used = 1;
    g_thread_info[idx].finished = 0;
    g_thread_info[idx].retval = NULL;

    /* Create a kernel thread — it shares the kernel address space
     * with the rest of the system, so CLONE_VM is implicit. */
    struct process *thread = kthread_create(thread_wrapper, tsa, "pthread");
    if (!thread) {
        kfree(tsa);
        g_thread_info[idx].used = 0;
        return -EINVAL;
    }

    /* Share thread group ID with parent */
    struct process *parent = current_process;
    if (parent) {
        thread->tgid = parent->tgid ? parent->tgid : parent->pid;
    }

    g_thread_info[idx].pid = (int)thread->pid;
    return (int)thread->pid;
}

/*
 * Wait for a thread to finish and retrieve its return value.
 * Returns 0 on success, -1 on error.
 */
int process_thread_join(int thread_pid, void **retval) {
    if (thread_pid <= 0)
        return -EINVAL;

    /* Find the thread info slot */
    int idx = -1;
    for (int i = 0; i < THREAD_INFO_MAX; i++) {
        if (g_thread_info[i].used && g_thread_info[i].pid == thread_pid) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return -EINVAL;

    /* Wait for the thread to finish by blocking on its PID.
     * We leverage the existing scheduler wake mechanism: when the
     * thread exits (becomes ZOMBIE), the scheduler yields back to us. */
    for (int spin = 0; spin < 10000000; spin++) {
        if (g_thread_info[idx].finished)
            break;
        /* Yield to let the thread run */
        scheduler_yield();
    }

    if (!g_thread_info[idx].finished) {
        /* Thread didn't finish — poll more aggressively */
        struct process *thr = process_get_by_pid((uint32_t)thread_pid);
        if (thr && thr->state == PROCESS_ZOMBIE) {
            g_thread_info[idx].finished = 1;
        } else {
            /* Thread still running — block until zombie */
            int status = 0;
            process_waitpid((uint32_t)thread_pid, &status, 0);
        }
    }

    /* Read the return value */
    if (retval) {
        *retval = g_thread_info[idx].retval;
    }

    /* Clean up */
    g_thread_info[idx].used = 0;

    /* If the thread is still alive, waitpid will clean it up.
     * Otherwise, it may already be a zombie. */
    struct process *thr = process_get_by_pid((uint32_t)thread_pid);
    if (thr && thr->state == PROCESS_ZOMBIE) {
        process_cleanup(thr);
    }

    return 0;
}

/*
 * Exit the current thread with the given return value.
 * Never returns.
 */
void process_thread_exit(void *retval) {
    int pid = current_process ? (int)current_process->pid : 0;

    /* Find and update the thread info slot */
    for (int i = 0; i < THREAD_INFO_MAX; i++) {
        if (g_thread_info[i].used && g_thread_info[i].pid == pid) {
            g_thread_info[i].retval = retval;
            g_thread_info[i].finished = 1;
            break;
        }
    }

    /* Become a zombie to notify the joiner */
    current_process->state = PROCESS_ZOMBIE;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);
    for (;;)
        __asm__ volatile("hlt"); /* never reached */
}

/* ── process_is_kthread / process_set_user_process ──────────── */

int process_is_kthread(struct process *proc) {
    if (!proc)
        return 0;
    return (proc->is_user == 0 && proc->pid > 0);
}

int process_set_user_process(uint64_t entry, uint64_t stack, uint64_t *pml4) {
    struct process *proc = process_get_current();
    if (!proc)
        return -EINVAL;

    /* Can only convert kernel threads to user processes */
    if (proc->is_user)
        return -EINVAL; /* already a user process */

    proc->is_user = 1;
    proc->user_entry = entry;
    proc->user_rsp = stack;
    proc->pml4 = pml4;

    /* Update capability profile for user execution */
    process_set_cap_profile(proc, PROCESS_CAP_PROFILE_USER_DEFAULT);

    return 0;
}

/* ── Module exports ──────────────────────────────────────────────── */
#include "export.h"
EXPORT_SYMBOL(process_exit);

/* ── Exec permission check (Item S14) ──────────────────────────────── */

/* Before executing a binary, verify that the inode has the necessary
 * execute permission bits (S_IXUSR, S_IXGRP, or S_IXOTH) based on
 * the current process's UID/GID.
 *
 * This is called from the exec path (do_execve in kernel/exec.c)
 * as an additional security check.
 *
 * @binary_path  Path to the binary file
 * @uid          Requesting user's UID (process euid)
 * @gid          Requesting user's GID (process egid)
 *
 * Returns 0 if execute is allowed, -EACCES if denied.
 */
int process_check_exec_perms(const char *binary_path, uint32_t uid, uint32_t gid) {
    if (!binary_path)
        return -EINVAL;

    /* Stat the binary to get its mode and ownership */
    struct vfs_stat st;
    int ret = vfs_stat(binary_path, &st);
    if (ret < 0)
        return -EACCES;

    uint16_t mode = st.mode;
    uint16_t file_uid = st.uid;
    uint16_t file_gid = st.gid;

    /* Check execute bits based on identity */
    if (uid == file_uid) {
        /* Owner check */
        if (!(mode & S_IXUSR))
            return -EACCES;
    } else if (gid == file_gid) {
        /* Group check */
        if (!(mode & S_IXGRP))
            return -EACCES;
    } else {
        /* Other check */
        if (!(mode & S_IXOTH))
            return -EACCES;
    }

    return 0; /* Execute permission granted */
}

/* ── process_wait ─────────────────────────────── */
static int process_wait(int pid, int *status, int options) {
    (void)options;
    struct process *cur = process_get_current();
    if (!cur)
        return -EINVAL;

    struct process *table = process_get_table();

    for (;;) {
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_ZOMBIE)
                continue;
            if (table[i].parent_pid != cur->pid)
                continue;
            if (pid > 0 && (int)table[i].pid != pid)
                continue;
            if (pid == -1)
                ; /* any child */
            else if (pid == 0) {
                /* Same process group as current */
                if (table[i].pgid != cur->pgid)
                    continue;
            }

            /* Found a zombie child */
            if (status)
                *status = table[i].exit_code;

            int child_pid = (int)table[i].pid;

            /* Clean up the child process */
            process_cleanup(&table[i]);
            table[i].state = PROCESS_UNUSED;

            return child_pid;
        }

        /* No zombie children found. If WNOHANG is set, return 0. */
        if (options & 1) /* WNOHANG */
            return 0;

        /* Check if any children exist at all */
        int has_children = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_UNUSED && table[i].parent_pid == cur->pid) {
                has_children = 1;
                break;
            }
        }

        if (!has_children)
            return -ECHILD;

        /* Block until a child exits */
        cur->state = PROCESS_BLOCKED;
        scheduler_remove(cur);
        scheduler_yield();
        /* Re-acquire and check again */
    }
}

/* ── process_kill ─────────────────────────────── */
static int process_kill(int pid, int sig) {
    if (pid <= 0)
        return -EINVAL;

    if (sig == 0) {
        /* Signal 0 is used to check if process exists */
        struct process *p = process_get_by_pid((uint32_t)pid);
        return (p && p->state != PROCESS_UNUSED) ? 0 : -ESRCH;
    }

    return signal_send((uint32_t)pid, sig);
}
