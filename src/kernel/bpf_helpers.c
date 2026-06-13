/*
 * bpf_helpers.c — Common eBPF helper functions
 *
 * Implements the helper functions that eBPF programs can call.
 * These are registered with the verifier and provide the interface
 * between eBPF programs and kernel functionality.
 *
 * Supported helpers:
 *   - map_lookup_elem / map_update_elem / map_delete_elem
 *   - ktime_get_ns
 *   - trace_printk
 *   - get_smp_processor_id
 *   - get_current_pid_tgid
 *
 * Item 134 — eBPF common helpers
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "smp.h"
#include "process.h"   /* for current process */
#include "bpf_maps.h"  /* internal map functions */

/* ── Map operation helpers ──────────────────────────────────────────── */

/*
 * Helper: map_lookup_elem(map_fd, key) -> value pointer or NULL.
 * Called from eBPF programs via BPF_CALL with imm = BPF_FUNC_map_lookup_elem.
 */
void *bpf_helper_map_lookup_elem(void *map_fd_ptr, void *key_ptr)
{
    /* Convert pointers to integer map fd */
    int map_fd = (int)(uintptr_t)map_fd_ptr;
    static uint64_t temp_val[16];  /* Temporary buffer for return value */

    int ret = bpf_map_lookup_elem(map_fd, key_ptr, temp_val);
    if (ret == 0)
        return temp_val;

    return NULL;
}

/*
 * Helper: map_update_elem(map_fd, key, value, flags) -> 0 or error.
 */
int bpf_helper_map_update_elem(void *map_fd_ptr, void *key_ptr,
                                void *value_ptr, void *flags_ptr)
{
    int map_fd = (int)(uintptr_t)map_fd_ptr;
    uint64_t flags = (uint64_t)(uintptr_t)flags_ptr;
    return bpf_map_update_elem(map_fd, key_ptr, value_ptr, flags);
}

/*
 * Helper: map_delete_elem(map_fd, key) -> 0 or error.
 */
int bpf_helper_map_delete_elem(void *map_fd_ptr, void *key_ptr)
{
    int map_fd = (int)(uintptr_t)map_fd_ptr;
    return bpf_map_delete_elem(map_fd, key_ptr);
}

/* ── Time helpers ───────────────────────────────────────────────────── */

/*
 * Helper: ktime_get_ns() -> current time in nanoseconds.
 */
uint64_t bpf_helper_ktime_get_ns(void)
{
    /* Convert timer ticks to nanoseconds */
    uint64_t ticks = timer_get_ticks();
    /* Assume TIMER_FREQ Hz, convert ticks to ns */
    return ticks * (1000000000ULL / 100);  /* 100 Hz default tick */
}

/* ── Printk helper ──────────────────────────────────────────────────── */

/*
 * Helper: trace_printk(fmt, fmt_size, ...) -> number of bytes written.
 *
 * Simple version: writes a formatted string to the kernel log/trace buffer.
 * For simplicity, this just calls kprintf.
 */
int bpf_helper_trace_printk(void *fmt_ptr, void *fmt_size_ptr, ...)
{
    /* Simple implementation: just kprintf a fixed message */
    kprintf("[bpf] trace_printk called\n");
    return 0;
}

/* ── CPU helpers ────────────────────────────────────────────────────── */

/*
 * Helper: get_smp_processor_id() -> current CPU number.
 */
uint32_t bpf_helper_get_smp_processor_id(void)
{
    return smp_get_cpu_id();
}

/*
 * Helper: get_current_pid_tgid() -> (pid << 32) | tgid.
 */
uint64_t bpf_helper_get_current_pid_tgid(void)
{
    struct task *current = get_current_task();
    if (!current) return 0;

    /* Construct combined pid/tgid */
    uint64_t result = (uint64_t)current->pid;
    result |= ((uint64_t)current->tgid) << 32;
    return result;
}

/* ── Helper function table ──────────────────────────────────────────── */

/* Maximum number of helper functions we can register */
#define BPF_MAX_HELPERS 64

/* Helper function signature */
typedef uint64_t (*bpf_helper_fn)(uint64_t r1, uint64_t r2, uint64_t r3,
                                   uint64_t r4, uint64_t r5);

/* Registered helper table */
static bpf_helper_fn bpf_helpers[BPF_MAX_HELPERS];
static int bpf_helpers_initialized = 0;

/*
 * Register a helper function.
 * @id:   Helper function ID (must match verifier expectations).
 * @fn:   Pointer to helper function.
 */
int bpf_helper_register(int id, bpf_helper_fn fn)
{
    if (!bpf_helpers_initialized)
        return -1;
    if (id < 0 || id >= BPF_MAX_HELPERS)
        return -1;

    bpf_helpers[id] = fn;
    return 0;
}

/*
 * Call a helper function by ID.
 */
uint64_t bpf_helper_call(int id, uint64_t r1, uint64_t r2, uint64_t r3,
                          uint64_t r4, uint64_t r5)
{
    if (id < 0 || id >= BPF_MAX_HELPERS || !bpf_helpers[id])
        return 0;

    return bpf_helpers[id](r1, r2, r3, r4, r5);
}

/*
 * Initialize the helper system and register built-in helpers.
 */
void bpf_helpers_init(void)
{
    if (bpf_helpers_initialized) return;

    memset(bpf_helpers, 0, sizeof(bpf_helpers));
    bpf_helpers_initialized = 1;

    /* Register built-in helpers with their canonical IDs */
    bpf_helper_register(1, (bpf_helper_fn)bpf_helper_map_lookup_elem);
    bpf_helper_register(2, (bpf_helper_fn)bpf_helper_map_update_elem);
    bpf_helper_register(3, (bpf_helper_fn)bpf_helper_map_delete_elem);
    bpf_helper_register(4, (bpf_helper_fn)bpf_helper_ktime_get_ns);
    bpf_helper_register(5, (bpf_helper_fn)bpf_helper_trace_printk);
    bpf_helper_register(6, (bpf_helper_fn)bpf_helper_get_smp_processor_id);
    bpf_helper_register(7, (bpf_helper_fn)bpf_helper_get_current_pid_tgid);

    kprintf("[bpf_helpers] Registered %d built-in helpers\n", 7);
}
