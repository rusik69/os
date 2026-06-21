/*
 * kcov.c — Kernel code coverage for fuzzing (Item 208)
 *
 * Provides coverage-guided feedback for fuzzers by recording basic-block
 * PC values into a per-task buffer.  Modeled after Linux KCOV.
 *
 * Usage (from userspace):
 *   syscall(SYS_KCOV, KCOV_INIT_TRACE, N);   // allocate N-entry buffer
 *   syscall(SYS_KCOV, KCOV_ENABLE, KCOV_TRACE_PC);  // start coverage
 *   ... exercise code paths (syscalls, etc.) ...
 *   syscall(SYS_KCOV, KCOV_DISABLE, 0);       // stop coverage
 *
 * The coverage buffer is a per-process array of uint64_t values.
 * Layout:
 *   [0] = number of valid entries
 *   [1] .. [N] = PC values of executed basic blocks
 */
#define KERNEL_INTERNAL
#include "types.h"
#include "kcov.h"
#include "process.h"
#include "heap.h"
#include "string.h"
#include "printf.h"

/* ── Forward declarations ─────────────────────────────────────────── */
static int kcov_init_trace(struct process *proc, uint64_t size);
static int kcov_enable(struct process *proc, uint64_t mode);
static int kcov_disable(struct process *proc);

/* ── Public API called from process init/exit ──────────────────────── */

void kcov_process_init(struct process *proc)
{
    if (!proc)
        return;
    proc->kcov_mode = KCOV_MODE_NONE;
    proc->kcov_size = 0;
    proc->kcov_area = NULL;
}

void kcov_process_exit(struct process *proc)
{
    if (!proc)
        return;
    if (proc->kcov_area) {
        kfree(proc->kcov_area);
        proc->kcov_area = NULL;
    }
    proc->kcov_mode = KCOV_MODE_NONE;
    proc->kcov_size = 0;
}

/* ── Record a coverage entry (called from instrumentation points) ─── */

void kcov_record(uint64_t pc)
{
    struct process *proc = process_get_current();
    if (!proc || proc->kcov_mode != KCOV_MODE_TRACE_PC)
        return;

    uint64_t *area = proc->kcov_area;
    if (!area)
        return;

    uint64_t n = area[0];  /* current number of valid entries */
    if (n >= proc->kcov_size - 1)
        return;  /* buffer full — silently drop */

    area[n + 1] = pc;
    area[0] = n + 1;
}

/* ── Syscall handler ───────────────────────────────────────────────── */

int sys_kcov(uint64_t cmd, uint64_t arg2)
{
    struct process *cur = process_get_current();
    if (!cur)
        return -1;

    switch (cmd) {
    case KCOV_INIT_TRACE:
        return kcov_init_trace(cur, arg2);

    case KCOV_ENABLE:
        return kcov_enable(cur, arg2);

    case KCOV_DISABLE:
        return kcov_disable(cur);

    default:
        return -1;  /* EINVAL */
    }
}

/* ── Internal helpers ──────────────────────────────────────────────── */

static int kcov_init_trace(struct process *proc, uint64_t size)
{
    if (proc->kcov_mode != KCOV_MODE_NONE)
        return -1;  /* EBUSY */

    if (size == 0 || size > KCOV_MAX_ENTRIES)
        return -1;  /* EINVAL */

    /* Allocate the coverage buffer.
     * Entries are uint64_t; we use one extra slot for the entry count at [0]. */
    uint64_t *area = (uint64_t *)kmalloc(size * sizeof(uint64_t));
    if (!area)
        return -1;  /* ENOMEM */

    memset(area, 0, size * sizeof(uint64_t));

    proc->kcov_area = area;
    proc->kcov_size = size;
    proc->kcov_mode = KCOV_MODE_INIT;

    return 0;
}

static int kcov_enable(struct process *proc, uint64_t mode)
{
    if (proc->kcov_mode != KCOV_MODE_INIT)
        return -1;  /* EINVAL (not initialised or already enabled) */

    if (mode != KCOV_TRACE_PC)
        return -1;  /* EINVAL (unsupported mode) */

    /* Reset the buffer: clear all entries, keep n=0 at [0] */
    if (proc->kcov_area)
        memset(proc->kcov_area, 0, proc->kcov_size * sizeof(uint64_t));

    proc->kcov_mode = KCOV_MODE_TRACE_PC;

    return 0;
}

static int kcov_disable(struct process *proc)
{
    if (proc->kcov_mode != KCOV_MODE_TRACE_PC)
        return -1;  /* EINVAL (not enabled) */

    proc->kcov_mode = KCOV_MODE_INIT;  /* back to initialised but inactive */
    return 0;
}

/* ── Stub: kcov_init ─────────────────────────────── */
int kcov_init(void)
{
    kprintf("[kcov] kcov_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kcov_open ─────────────────────────────── */
int kcov_open(void *file)
{
    (void)file;
    kprintf("[kcov] kcov_open: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kcov_ioctl ─────────────────────────────── */
int kcov_ioctl(void *file, int cmd, unsigned long arg)
{
    (void)file;
    (void)cmd;
    (void)arg;
    kprintf("[kcov] kcov_ioctl: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kcov_collect ─────────────────────────────── */
int kcov_collect(void *task, void *buf, size_t len)
{
    (void)task;
    (void)buf;
    (void)len;
    kprintf("[kcov] kcov_collect: not yet implemented\n");
    return -ENOSYS;
}
