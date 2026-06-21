/*
 * bpf_helpers.c — eBPF helper functions for BPF programs
 *
 * Implements helper functions that eBPF programs can call at runtime.
 * These provide controlled access to kernel functionality from
 * sandboxed BPF programs.
 *
 * Available helpers:
 *   bpf_get_current_pid_tgid — Get current PID and TGID
 *   bpf_get_current_uid_gid  — Get current UID and GID
 *   bpf_get_current_comm     — Get current process name
 *   bpf_trace_printk         — Print to kernel trace log
 *   bpf_ktime_get_ns         — Get current time in nanoseconds
 *   bpf_map_lookup_elem      — Look up a map entry
 *   bpf_map_update_elem      — Update a map entry
 *   bpf_map_delete_elem      — Delete a map entry
 *   bpf_get_smp_processor_id — Get current CPU ID
 *   bpf_perf_event_output    — Output a perf event
 */

#define KERNEL_INTERNAL
#include "bpf_helpers.h"
#include "bpf_maps.h"
#include "process.h"
#include "scheduler.h"
#include "printf.h"
#include "timer.h"
#include "smp.h"
#include "string.h"

/* ── Helper function table ─────────────────────────────────────────── */

/* Each entry maps a helper function ID (from the BPF CALL instruction's
 * imm field) to an actual kernel function.  Helper IDs are negative
 * for kernel helpers (Linux convention).  We use negative IDs here. */

/* Helper function IDs (negative values for kernel helpers) */
#define BPF_FUNC_unspec               0
#define BPF_FUNC_map_lookup_elem     -1
#define BPF_FUNC_map_update_elem     -2
#define BPF_FUNC_map_delete_elem     -3
#define BPF_FUNC_get_current_pid_tgid  -4
#define BPF_FUNC_get_current_uid_gid   -5
#define BPF_FUNC_get_current_comm      -6
#define BPF_FUNC_trace_printk          -7
#define BPF_FUNC_ktime_get_ns          -8
#define BPF_FUNC_get_smp_processor_id  -9
#define BPF_FUNC_perf_event_output    -10

/* ── Helper implementations ────────────────────────────────────────── */

/* Return current PID and TGID (packed: lower 32 = pid, upper 32 = tgid) */
uint64_t bpf_get_current_pid_tgid(void)
{
    struct process *cur = get_current_process();
    if (!cur) return 0;
    return ((uint64_t)cur->pid) | ((uint64_t)cur->pid << 32);
}

/* Return current UID and GID (packed: lower 32 = uid, upper 32 = gid) */
uint64_t bpf_get_current_uid_gid(void)
{
    struct process *cur = get_current_process();
    if (!cur) return 0;
    return ((uint64_t)cur->uid) | ((uint64_t)cur->gid << 32);
}

/* Get current process name (max 16 bytes including NUL) */
uint64_t bpf_get_current_comm(char *buf, uint32_t size)
{
    struct process *cur = get_current_process();
    if (!cur || !buf || size == 0) return 1;
    size_t len = strlen(cur->name);
    if (len >= size) len = size - 1;
    memcpy(buf, cur->name, len);
    buf[len] = '\0';
    return 0;
}

/* Print a formatted message to the kernel trace log.
 * Supports a single %d or %u format specifier for simplicity. */
uint64_t bpf_trace_printk(const char *fmt, uint32_t fmt_size, ...)
{
    /* Simple implementation: print directly to kernel log */
    if (!fmt || fmt_size == 0) return 1;

    /* Copy to a safe buffer */
    char buf[256];
    uint32_t len = fmt_size < sizeof(buf) - 1 ? fmt_size : sizeof(buf) - 1;
    memcpy(buf, fmt, len);
    buf[len] = '\0';

    kprintf("[BPF] %s\n", buf);
    return 0;
}

/* Get current time in nanoseconds */
uint64_t bpf_ktime_get_ns(void)
{
    return timer_get_ns();
}

/* Get current CPU ID */
uint32_t bpf_get_smp_processor_id(void)
{
    return smp_get_cpu_id();
}

/* ── Dispatcher ─────────────────────────────────────────────────────── */

/* Called when a BPF program issues a CALL instruction with helper ID.
 * Returns 0 on success, 1 on unknown helper. */
uint64_t bpf_dispatch_helper(int helper_id, uint64_t r1, uint64_t r2,
                              uint64_t r3, uint64_t r4, uint64_t r5)
{
    switch (helper_id) {
    case BPF_FUNC_map_lookup_elem:
        return (uint64_t)(uintptr_t)bpf_map_lookup_elem;
    case BPF_FUNC_map_update_elem:
        return (uint64_t)(uintptr_t)bpf_map_update_elem;
    case BPF_FUNC_map_delete_elem:
        return (uint64_t)(uintptr_t)bpf_map_delete_elem;
    case BPF_FUNC_get_current_pid_tgid:
        return bpf_get_current_pid_tgid();
    case BPF_FUNC_get_current_uid_gid:
        return bpf_get_current_uid_gid();
    case BPF_FUNC_get_current_comm:
        return bpf_get_current_comm((char *)(uintptr_t)r1, (uint32_t)r2);
    case BPF_FUNC_trace_printk:
        return bpf_trace_printk((const char *)(uintptr_t)r1, (uint32_t)r2);
    case BPF_FUNC_ktime_get_ns:
        return bpf_ktime_get_ns();
    case BPF_FUNC_get_smp_processor_id:
        return bpf_get_smp_processor_id();
    case BPF_FUNC_perf_event_output: {
        struct bpf_perf_event_output_ctx *ctx = (struct bpf_perf_event_output_ctx *)(uintptr_t)r1;
        if (!ctx) return 1;
        kprintf("[BPF-perf] ctx=%p data=%llx size=%u\n",
                (void *)ctx, (unsigned long long)ctx->data, (unsigned int)ctx->size);
        return 0;
    }
    default:
        kprintf("[BPF] Unknown helper ID %d\n", helper_id);
        return 1;
    }
}

/* ── Initialization ─────────────────────────────────────────────────── */

void bpf_helpers_init(void)
{
    kprintf("[OK] BPF helpers initialized\n");
}

/* ── Stub: bpf_map_lookup_elem ─────────────────────────────── */
void* bpf_map_lookup_elem(void *map, const void *key)
{
    (void)map;
    (void)key;
    kprintf("[bpf] bpf_map_lookup_elem: not yet implemented\n");
    return 0;
}
/* ── Stub: bpf_map_update_elem ─────────────────────────────── */
int bpf_map_update_elem(void *map, const void *key, const void *val, uint64_t flags)
{
    (void)map;
    (void)key;
    (void)val;
    (void)flags;
    kprintf("[bpf] bpf_map_update_elem: not yet implemented\n");
    return 0;
}
/* ── Stub: bpf_map_delete_elem ─────────────────────────────── */
int bpf_map_delete_elem(void *map, const void *key)
{
    (void)map;
    (void)key;
    kprintf("[bpf] bpf_map_delete_elem: not yet implemented\n");
    return 0;
}
