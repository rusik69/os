#ifndef TRACEFS_H
#define TRACEFS_H

#include "types.h"

/*
 * tracefs — trace virtual filesystem mounted at /sys/kernel/trace.
 *
 * Provides:
 *   - Per-CPU ring buffers for kernel trace data
 *   - /sys/kernel/trace/tracing_on — enable/disable tracing
 *   - /sys/kernel/trace/trace_marker — write trace entries
 *   - /sys/kernel/trace/per_cpu/cpu<N>/trace — per-CPU buffer reads
 *   - /sys/kernel/trace/buffer_size_kb — per-CPU buffer size control
 *
 * Per-CPU trace buffers are fixed-size ring buffers.  Writers append
 * trace entries; readers consume them.  The trace is lock-free in the
 * fast path using a per-CPU write pointer and atomic updates.
 */

/* Maximum CPUs we allocate buffers for */
#define TRACEFS_MAX_CPUS    16

/* Default per-CPU buffer size (16 KB) */
#define TRACEFS_BUF_SIZE    16384

/* Minimum per-CPU buffer size (2 KB) */
#define TRACEFS_BUF_MIN     2048

/* Maximum per-CPU buffer size (128 KB) */
#define TRACEFS_BUF_MAX     131072

/* Maximum path length for VFS operations */
#define TRACEFS_MAX_PATH    64

/* Maximum entries in the tracefs entry table */
#define TRACEFS_MAX_ENTRIES 128

/* Entry type constants */
#define TRACEFS_TYPE_DIR    2
#define TRACEFS_TYPE_FILE   1

/* ── Per-CPU trace buffer ───────────────────────────────────────────── */

struct tracefs_percpu_buf {
    char    *data;           /* ring buffer data */
    uint32_t size;           /* buffer size in bytes */
    uint32_t write_pos;      /* next write position (written by owning CPU) */
    uint32_t read_pos;       /* read position (updated by consumer) */
    spinlock_t lock;         /* lock for read-side synchronisation */
};

/* ── Tracefs entry (VFS-visible) ───────────────────────────────────── */

struct tracefs_entry {
    char     name[48];
    uint8_t  type;           /* TRACEFS_TYPE_FILE or TRACEFS_TYPE_DIR */
    int      parent;         /* index of parent (-1 for root) */
    int      in_use;
    /* File-specific callbacks */
    void (*read_fn)(char *buf, int *len, void *priv);
    int  (*write_fn)(const char *buf, int len, void *priv);
    void *priv;              /* private data for callbacks */
};

/* ── Public API ────────────────────────────────────────────────────── */

/* Initialise tracefs — allocates per-CPU buffers and mounts */
void tracefs_init(void);

/* Write a trace entry to the current CPU's ring buffer.
 * Returns 0 on success, negative errno on error. */
int tracefs_write_entry(const char *data, uint32_t len);

/* Enable/disable tracing globally. */
void tracefs_enable(void);
void tracefs_disable(void);
int  tracefs_is_enabled(void);

/* Get per-CPU buffer fill level (bytes available to read). */
uint32_t tracefs_percpu_avail(int cpu_id);

/* VFS operations table (extern for registration) */
extern struct vfs_ops tracefs_vfs_ops;

#endif /* TRACEFS_H */
