#ifndef UPROBES_H
#define UPROBES_H

#include "types.h"
#include "process.h"

/* ── Uprobe constants ──────────────────────────────────────────────── */

#define UPROBES_MAX_PROBES    64   /* maximum number of registered probes */
#define UPROBES_HASH_BITS     6    /* 64-entry hash table (2^6) */
#define UPROBES_PATH_MAX      128  /* max path length for target file */
#define UPROBES_RING_SIZE     256  /* entries in the event ring buffer */

/* Uprobe handler callback type.
 * @pid:      PID of the probed process
 * @address:  Virtual address where the breakpoint was hit
 * @timestamp: Timer tick when the event occurred
 */
typedef void (*uprobe_handler_t)(uint32_t pid, uint64_t address,
                                  uint64_t timestamp);

/* ── Uprobe structure ───────────────────────────────────────────────── */

struct uprobe {
    char      path[UPROBES_PATH_MAX];  /* path to the target executable */
    uint64_t  offset;                   /* file offset of the probe point */
    uint64_t  vaddr;                    /* virtual address (resolved on first hit) */
    uint8_t   orig_byte;               /* original byte at the probe point (replaced by 0xCC) */
    int       active;                   /* 1 if the breakpoint is inserted */
    uprobe_handler_t handler;           /* callback function on hit */
    uint32_t  flags;                    /* reserved */
    int       in_use;                   /* slot allocated */
};

/* ── Uprobe event (ring buffer entry) ────────────────────────────────── */

struct uprobe_event {
    uint32_t pid;           /* PID that hit the breakpoint */
    uint64_t address;       /* virtual address of the breakpoint */
    uint64_t timestamp;     /* timer tick when the event occurred */
    int      valid;         /* 1 if this entry is valid */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise the uprobes subsystem. */
void uprobes_init(void);

/* Register a uprobe: insert a breakpoint at @offset in the file @path.
 * When the breakpoint is hit, @handler is called.
 * Returns 0 on success, negative errno on failure.
 */
int uprobe_register(const char *path, uint64_t offset,
                    uprobe_handler_t handler);

/* Unregister a uprobe: remove the breakpoint at @offset in @path.
 * Returns 0 on success, negative errno on failure.
 */
int uprobe_unregister(const char *path, uint64_t offset);

/* Called from the breakpoint handler (int3, vector 3) when a
 * uprobe breakpoint is hit.  This is the core handler:
 *   1. Identify which uprobe was hit
 *   2. Execute the handler callback
 *   3. Single-step the original instruction
 *   4. Re-insert the breakpoint
 * Returns 1 if the fault was handled (probe found), 0 if not a uprobe.
 */
int uprobe_handle_breakpoint(struct interrupt_frame *frame);

/* Debugfs read callback for /sys/kernel/debug/uprobes/list */
void uprobe_list_read(char *buf, int *len);

/* Debugfs write callback for /sys/kernel/debug/uprobes/register */
int uprobe_register_write(const char *buf, int len);

#endif /* UPROBES_H */
