/*
 * edac.c — EDAC (Error Detection and Correction) for DRAM ECC
 *
 * Implements memory error detection and reporting for systems with
 * ECC (Error Correcting Code) DRAM.  The EDAC subsystem:
 *   - Probes for ECC-capable memory controllers
 *   - Monitors for correctable (CE) and uncorrectable (UE) errors
 *   - Provides a mechanism for error notification and logging
 *   - Tracks error counts per memory controller/channel
 *   - Supports polling-based error detection
 *
 * Item 457: EDAC — DRAM ECC error detection
 */

#define KERNEL_INTERNAL
#include "edac.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define EDAC_MAX_CONTROLLERS  4   /* max memory controllers */
#define EDAC_MAX_CSROWS       8   /* max chip-select rows per controller */
#define EDAC_MAX_CHANNELS     4   /* max channels per controller */
#define EDAC_POLL_INTERVAL_MS 1000 /* default polling interval */
#define EDAC_CE_THRESHOLD     100  /* threshold for CE flooding detection */

/* Memory controller types */
#define EDAC_CTL_UNKNOWN  0
#define EDAC_CTL_SBRIDGE  1   /* Intel Sandy Bridge / Ivy Bridge */
#define EDAC_CTL_IBRIDGE  2   /* Intel Ivy Bridge */
#define EDAC_CTL_HASWELL  3   /* Intel Haswell */
#define EDAC_CTL_SKX      4   /* Intel Skylake-X */
#define EDAC_CTL_ZEN      5   /* AMD Zen family */

/* Error types */
#define EDAC_ERR_NONE     0
#define EDAC_ERR_CE       1   /* Correctable Error */
#define EDAC_ERR_UE       2   /* Uncorrectable Error */
#define EDAC_ERR_FATAL    3   /* Fatal / system-critical error */

/* ── Chip-select row ──────────────────────────────────────────────── */

struct edac_csrow {
    int         in_use;
    uint64_t    ce_count;       /* correctable error count */
    uint64_t    ue_count;       /* uncorrectable error count */
    uint64_t    last_ce_tick;   /* tick of last CE */
    uint64_t    last_ue_tick;   /* tick of last UE */
    int         ce_flooding;    /* 1 if CE rate exceeds threshold */
};

/* ── Memory channel ───────────────────────────────────────────────── */

struct edac_channel {
    int         in_use;
    uint64_t    ce_count;
    uint64_t    ue_count;
    int         diag_messages;  /* number of diagnostic messages printed */
};

/* ── Memory controller ────────────────────────────────────────────── */

struct edac_controller {
    int     in_use;
    int     type;                   /* EDAC_CTL_* */
    char    name[32];
    int     num_csrows;
    int     num_channels;

    struct edac_csrow    csrows[EDAC_MAX_CSROWS];
    struct edac_channel  channels[EDAC_MAX_CHANNELS];

    uint64_t total_ce;              /* total CE count for this controller */
    uint64_t total_ue;              /* total UE count */
    uint64_t last_poll_tick;        /* last polling tick */

    /* Callbacks */
    int (*poll_fn)(struct edac_controller *);  /* polling function */
    void (*ce_handler)(int ctl, int csrow, int channel, uint64_t addr);
    void (*ue_handler)(int ctl, int csrow, int channel, uint64_t addr);
};

/* ── Global state ─────────────────────────────────────────────────── */

static struct edac_controller g_controllers[EDAC_MAX_CONTROLLERS];
static int g_num_controllers = 0;
static int g_edac_initialized = 0;
static spinlock_t g_edac_lock;

/* ── Default error handlers ───────────────────────────────────────── */

static void default_ce_handler(int ctl, int csrow, int channel, uint64_t addr)
{
    kprintf("[EDAC] CE: controller=%d csrow=%d channel=%d addr=0x%llx\n",
            ctl, csrow, channel, (unsigned long long)addr);
}

static void default_ue_handler(int ctl, int csrow, int channel, uint64_t addr)
{
    kprintf("[EDAC] UE (FATAL): controller=%d csrow=%d channel=%d addr=0x%llx\n",
            ctl, csrow, channel, (unsigned long long)addr);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize the EDAC subsystem. */
void __init edac_init(void)
{
    if (g_edac_initialized) return;

    memset(g_controllers, 0, sizeof(g_controllers));
    spinlock_init(&g_edac_lock);
    g_edac_initialized = 1;

    kprintf("[OK] EDAC: DRAM ECC error detection initialized\n");
}
EXPORT_SYMBOL(edac_init);

/* Register a memory controller for EDAC monitoring.
 *
 * @name:   controller name (e.g., "i7core_edac0")
 * @type:   controller type (EDAC_CTL_*)
 * @poll_fn: polling callback (called periodically to check for errors)
 *
 * Returns controller ID (>= 0) on success, negative on failure.
 */
int edac_register_controller(const char *name, int type,
                             int (*poll_fn)(struct edac_controller *))
{
    if (!g_edac_initialized) return -EAGAIN;
    if (!name || type < 0) return -EINVAL;

    spinlock_acquire(&g_edac_lock);
    if (g_num_controllers >= EDAC_MAX_CONTROLLERS) {
        spinlock_release(&g_edac_lock);
        return -ENOSPC;
    }

    int id = g_num_controllers;
    struct edac_controller *ctl = &g_controllers[id];
    memset(ctl, 0, sizeof(*ctl));
    ctl->in_use = 1;
    strncpy(ctl->name, name, sizeof(ctl->name) - 1);
    ctl->name[sizeof(ctl->name) - 1] = '\0';
    ctl->type = type;
    ctl->num_csrows = 2;   /* default */
    ctl->num_channels = 2;
    ctl->poll_fn = poll_fn;
    ctl->ce_handler = default_ce_handler;
    ctl->ue_handler = default_ue_handler;
    ctl->last_poll_tick = 0;
    g_num_controllers++;

    spinlock_release(&g_edac_lock);

    kprintf("[EDAC] registered controller '%s' (type %d) as id %d\n",
            name, type, id);
    return id;
}
EXPORT_SYMBOL(edac_register_controller);

/* Unregister a memory controller. */
int edac_unregister_controller(int ctl_id)
{
    if (ctl_id < 0 || ctl_id >= g_num_controllers ||
        !g_controllers[ctl_id].in_use)
        return -EINVAL;

    spinlock_acquire(&g_edac_lock);
    memset(&g_controllers[ctl_id], 0, sizeof(struct edac_controller));
    spinlock_release(&g_edac_lock);
    return 0;
}
EXPORT_SYMBOL(edac_unregister_controller);

/* Report a correctable error (CE).
 *
 * @ctl_id:   controller ID
 * @csrow:    chip-select row (or -1 if unknown)
 * @channel:  memory channel (or -1 if unknown)
 * @addr:     physical address of the error (or 0 if unknown)
 */
void edac_ce_error(int ctl_id, int csrow, int channel, uint64_t addr)
{
    if (ctl_id < 0 || ctl_id >= EDAC_MAX_CONTROLLERS ||
        !g_controllers[ctl_id].in_use)
        return;

    struct edac_controller *ctl = &g_controllers[ctl_id];
    spinlock_acquire(&g_edac_lock);

    ctl->total_ce++;

    /* Update csrow stats */
    if (csrow >= 0 && csrow < EDAC_MAX_CSROWS && ctl->csrows[csrow].in_use) {
        ctl->csrows[csrow].ce_count++;
        ctl->csrows[csrow].last_ce_tick = timer_get_ticks();
    }

    /* Update channel stats */
    if (channel >= 0 && channel < EDAC_MAX_CHANNELS && ctl->channels[channel].in_use) {
        ctl->channels[channel].ce_count++;
    }

    spinlock_release(&g_edac_lock);

    /* Call error handler */
    if (ctl->ce_handler)
        ctl->ce_handler(ctl_id, csrow, channel, addr);
}
EXPORT_SYMBOL(edac_ce_error);

/* Report an uncorrectable error (UE).
 *
 * @ctl_id:   controller ID
 * @csrow:    chip-select row (or -1 if unknown)
 * @channel:  memory channel (or -1 if unknown)
 * @addr:     physical address of the error (or 0 if unknown)
 */
void edac_ue_error(int ctl_id, int csrow, int channel, uint64_t addr)
{
    if (ctl_id < 0 || ctl_id >= EDAC_MAX_CONTROLLERS ||
        !g_controllers[ctl_id].in_use)
        return;

    struct edac_controller *ctl = &g_controllers[ctl_id];
    spinlock_acquire(&g_edac_lock);

    ctl->total_ue++;

    if (csrow >= 0 && csrow < EDAC_MAX_CSROWS && ctl->csrows[csrow].in_use) {
        ctl->csrows[csrow].ue_count++;
        ctl->csrows[csrow].last_ue_tick = timer_get_ticks();
    }

    if (channel >= 0 && channel < EDAC_MAX_CHANNELS && ctl->channels[channel].in_use) {
        ctl->channels[channel].ue_count++;
    }

    spinlock_release(&g_edac_lock);

    if (ctl->ue_handler)
        ctl->ue_handler(ctl_id, csrow, channel, addr);
}
EXPORT_SYMBOL(edac_ue_error);

/* Configue CSROW layout for a controller. */
int edac_setup_csrows(int ctl_id, int num_csrows)
{
    if (ctl_id < 0 || ctl_id >= EDAC_MAX_CONTROLLERS ||
        !g_controllers[ctl_id].in_use)
        return -EINVAL;

    if (num_csrows < 1 || num_csrows > EDAC_MAX_CSROWS)
        return -EINVAL;

    struct edac_controller *ctl = &g_controllers[ctl_id];
    spinlock_acquire(&g_edac_lock);
    ctl->num_csrows = num_csrows;
    for (int i = 0; i < num_csrows; i++)
        ctl->csrows[i].in_use = 1;
    spinlock_release(&g_edac_lock);
    return 0;
}
EXPORT_SYMBOL(edac_setup_csrows);

/* Set custom error handlers for a controller. */
void edac_set_handlers(int ctl_id,
                       void (*ce_fn)(int, int, int, uint64_t),
                       void (*ue_fn)(int, int, int, uint64_t))
{
    if (ctl_id < 0 || ctl_id >= EDAC_MAX_CONTROLLERS ||
        !g_controllers[ctl_id].in_use)
        return;

    struct edac_controller *ctl = &g_controllers[ctl_id];
    spinlock_acquire(&g_edac_lock);
    if (ce_fn) ctl->ce_handler = ce_fn;
    if (ue_fn) ctl->ue_handler = ue_fn;
    spinlock_release(&g_edac_lock);
}
EXPORT_SYMBOL(edac_set_handlers);

/* Poll all registered controllers for errors.
 * Called periodically from the EDAC workqueue or timer.
 * Returns total number of errors detected. */
int edac_poll_all(void)
{
    if (!g_edac_initialized) return 0;

    int total_errors = 0;
    for (int i = 0; i < g_num_controllers; i++) {
        if (!g_controllers[i].in_use) continue;
        if (g_controllers[i].poll_fn) {
            g_controllers[i].last_poll_tick = timer_get_ticks();
            int errs = g_controllers[i].poll_fn(&g_controllers[i]);
            if (errs > 0) total_errors += errs;
        }
    }
    return total_errors;
}
EXPORT_SYMBOL(edac_poll_all);

/* Get EDAC statistics for a controller. */
int edac_get_stats(int ctl_id, uint64_t *ce, uint64_t *ue, int *type)
{
    if (ctl_id < 0 || ctl_id >= EDAC_MAX_CONTROLLERS ||
        !g_controllers[ctl_id].in_use)
        return -EINVAL;

    struct edac_controller *ctl = &g_controllers[ctl_id];
    if (ce)   *ce   = ctl->total_ce;
    if (ue)   *ue   = ctl->total_ue;
    if (type) *type = ctl->type;
    return 0;
}
EXPORT_SYMBOL(edac_get_stats);

/* Dump EDAC status for all controllers. */
void edac_dump_status(void)
{
    if (!g_edac_initialized) return;

    kprintf("=== EDAC Status ===\n");
    spinlock_acquire(&g_edac_lock);
    for (int i = 0; i < g_num_controllers; i++) {
        if (!g_controllers[i].in_use) continue;
        struct edac_controller *ctl = &g_controllers[i];
        kprintf("Controller %d: %s (type %d)\n", i, ctl->name, ctl->type);
        kprintf("  CE: %llu  UE: %llu\n",
                (unsigned long long)ctl->total_ce,
                (unsigned long long)ctl->total_ue);
        for (int c = 0; c < ctl->num_csrows && c < EDAC_MAX_CSROWS; c++) {
            if (ctl->csrows[c].in_use) {
                kprintf("  CSROW %d: CE=%llu UE=%llu\n", c,
                        (unsigned long long)ctl->csrows[c].ce_count,
                        (unsigned long long)ctl->csrows[c].ue_count);
            }
        }
        for (int ch = 0; ch < ctl->num_channels && ch < EDAC_MAX_CHANNELS; ch++) {
            if (ctl->channels[ch].in_use) {
                kprintf("  CHANNEL %d: CE=%llu UE=%llu\n", ch,
                        (unsigned long long)ctl->channels[ch].ce_count,
                        (unsigned long long)ctl->channels[ch].ue_count);
            }
        }
    }
    spinlock_release(&g_edac_lock);
}
EXPORT_SYMBOL(edac_dump_status);
#include "module.h"
module_init(edac_init);

/* ── Stub: edac_report_error ─────────────────────────────── */
static int edac_report_error(void *dev, int type, const char *msg)
{
    (void)dev;
    (void)type;
    (void)msg;
    kprintf("[EDAC] edac_report_error: not yet implemented\n");
    return 0;
}
