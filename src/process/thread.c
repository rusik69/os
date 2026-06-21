/*
 * thread.c — Thread management and TLS area setup
 *
 * Implements per-thread data structures and Thread-Local Storage (TLS)
 * area management. Each thread gets a TLS area pointed to by the FS
 * segment base (on x86_64), allocated from the kernel heap.
 *
 * TLS areas are used for errno, per-thread signal masks, and other
 * thread-local variables.
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "process.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "cpu.h"
#include "scheduler.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define THREAD_MAX_TLS_SIZE   4096    /* max TLS area per thread */
#define THREAD_MAX_NAME        64
#define THREAD_MAX             256    /* max total threads system-wide */

/* ── Thread descriptor ─────────────────────────────────────────────────── */

struct thread {
    int      in_use;
    int      tid;                      /* thread ID (unique) */
    int      pid;                      /* owning process */
    uint8_t  *tls_area;                /* thread-local storage area */
    uint32_t tls_size;                 /* allocated TLS size */
    uint64_t fs_base;                  /* FS.base MSR value */
    char     name[THREAD_MAX_NAME];
};

/* ── Global state ──────────────────────────────────────────────────────── */

static struct thread thread_table[THREAD_MAX];
static int thread_count;
static int next_tid;
static spinlock_t thread_lock;
static int thread_initialised;

/* ── Initialisation ────────────────────────────────────────────────────── */

void thread_init(void)
{
    if (thread_initialised)
        return;

    memset(thread_table, 0, sizeof(thread_table));
    thread_count = 0;
    next_tid = 1;
    spinlock_init(&thread_lock);
    thread_initialised = 1;

    kprintf("[thread] Thread subsystem initialised (max %d threads, %d KB TLS)\n",
            THREAD_MAX, THREAD_MAX_TLS_SIZE / 1024);
}

/* ── TLS area allocation ───────────────────────────────────────────────── */

/* Allocate a TLS area for a thread. The area is zero-initialised
 * and aligned to 16 bytes. */
static int tls_area_alloc(struct thread *t, uint32_t size)
{
    if (size == 0)
        size = 64;  /* minimum: enough for errno + basics */

    if (size > THREAD_MAX_TLS_SIZE)
        size = THREAD_MAX_TLS_SIZE;

    t->tls_area = (uint8_t *)kmalloc(size);
    if (!t->tls_area)
        return -1;

    memset(t->tls_area, 0, size);
    t->tls_size = size;
    return 0;
}

/* Free a thread's TLS area */
static void tls_area_free(struct thread *t)
{
    if (t->tls_area) {
        kfree(t->tls_area);
        t->tls_area = NULL;
    }
    t->tls_size = 0;
}

/* Set FS.base to point to the thread's TLS area.
 * This is how user-space code accesses thread-local variables. */
static void thread_set_fs_base(uint64_t addr)
{
    /* Write IA32_FS_BASE MSR (0xC0000100) */
    uint32_t lo = (uint32_t)(addr & 0xFFFFFFFFULL);
    uint32_t hi = (uint32_t)(addr >> 32);
    __asm__ volatile("wrmsr" : : "c"(0xC0000100ULL), "a"(lo), "d"(hi));
}

/* ── Thread creation ───────────────────────────────────────────────────── */

/* Create a new thread within a process.
 * Returns thread ID (tid) on success, -1 on failure. */
int thread_create(int pid, const char *name, uint32_t tls_size)
{
    if (!thread_initialised)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&thread_lock, &irq_flags);

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < THREAD_MAX; i++) {
        if (!thread_table[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&thread_lock, irq_flags);
        return -1;
    }

    struct thread *t = &thread_table[slot];
    t->in_use = 1;
    t->tid = next_tid++;
    t->pid = pid;
    t->fs_base = 0;

    if (name)
        strncpy(t->name, name, THREAD_MAX_NAME - 1);
    else
        snprintf(t->name, THREAD_MAX_NAME, "thread-%d", t->tid);
    t->name[THREAD_MAX_NAME - 1] = '\0';

    /* Allocate TLS area */
    if (tls_area_alloc(t, tls_size) < 0) {
        t->in_use = 0;
        spinlock_irqsave_release(&thread_lock, irq_flags);
        return -1;
    }

    /* Set FS.base to point to TLS area */
    t->fs_base = (uint64_t)(uintptr_t)t->tls_area;
    thread_set_fs_base(t->fs_base);

    thread_count++;
    kprintf("[thread] Created thread %d '%s' (pid=%d, TLS=%u bytes at 0x%llx)\n",
            t->tid, t->name, pid, t->tls_size,
            (unsigned long long)t->fs_base);

    spinlock_irqsave_release(&thread_lock, irq_flags);
    return t->tid;
}

/* ── Thread destruction ────────────────────────────────────────────────── */

int thread_destroy(int tid)
{
    if (!thread_initialised || tid <= 0)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&thread_lock, &irq_flags);

    for (int i = 0; i < THREAD_MAX; i++) {
        if (thread_table[i].in_use && thread_table[i].tid == tid) {
            struct thread *t = &thread_table[i];
            tls_area_free(t);
            memset(t, 0, sizeof(*t));
            thread_count--;
            spinlock_irqsave_release(&thread_lock, irq_flags);
            return 0;
        }
    }

    spinlock_irqsave_release(&thread_lock, irq_flags);
    return -ENOENT;
}

/* ── Thread lookup ─────────────────────────────────────────────────────── */

int thread_find_by_tid(int tid)
{
    for (int i = 0; i < THREAD_MAX; i++) {
        if (thread_table[i].in_use && thread_table[i].tid == tid)
            return i;
    }
    return -1;
}

int thread_get_tid(int pid)
{
    /* Return first thread found for this pid */
    for (int i = 0; i < THREAD_MAX; i++) {
        if (thread_table[i].in_use && thread_table[i].pid == pid)
            return thread_table[i].tid;
    }
    return -1;
}

/* ── TLS accessors ─────────────────────────────────────────────────────── */

/* Get the TLS area address for a thread.
 * Returns virtual address of TLS area, or NULL if thread not found. */
void *thread_get_tls(int tid)
{
    int slot = thread_find_by_tid(tid);
    if (slot < 0)
        return NULL;
    return thread_table[slot].tls_area;
}

/* Set the TLS area FS.base for the current thread.
 * Called on context switch to update FS.base for the new thread. */
void thread_switch_to(int tid)
{
    int slot = thread_find_by_tid(tid);
    if (slot < 0)
        return;

    struct thread *t = &thread_table[slot];
    if (t->fs_base != 0)
        thread_set_fs_base(t->fs_base);
}

/* Get the FS.base value for a thread (for context save) */
uint64_t thread_get_fs_base(int tid)
{
    int slot = thread_find_by_tid(tid);
    if (slot < 0)
        return 0;
    return thread_table[slot].fs_base;
}

/* ── Stub: thread_exit ─────────────────────────────── */
int thread_exit(void *task)
{
    (void)task;
    kprintf("[thread] thread_exit: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: thread_yield ─────────────────────────────── */
int thread_yield(void)
{
    kprintf("[thread] thread_yield: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: thread_sleep ─────────────────────────────── */
int thread_sleep(uint64_t ns)
{
    (void)ns;
    kprintf("[thread] thread_sleep: not yet implemented\n");
    return -ENOSYS;
}
