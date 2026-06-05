/*
 * userfaultfd.c — Userfaultfd with range tracking and SIGBUS support (Item 134)
 *
 * Features:
 *   - Track registered address ranges per context (UFFD_FLAG_MISSING, WP, MINOR)
 *   - UFFD_FEATURE_SIGBUS: deliver SIGBUS instead of queuing events
 *   - Minor fault mode for page cache faults
 *   - Event ring buffer with read interface
 */

#include "userfaultfd.h"
#include "string.h"
#include "kernel.h"
#include "heap.h"
#include "printf.h"
#include "errno.h"
#include "process.h"
#include "signal.h"
#include "vmm.h"

static struct uffd_context uffd_table[UFFD_MAX_CONTEXTS];
static int uffd_initialised = 0;

void uffd_init(void)
{
    if (uffd_initialised)
        return;

    for (int i = 0; i < UFFD_MAX_CONTEXTS; i++) {
        struct uffd_context *ctx = &uffd_table[i];
        spinlock_init(&ctx->lock);
        ctx->used = 0;
        ctx->fd = i;
        ctx->event_head = 0;
        ctx->event_tail = 0;
        ctx->num_ranges = 0;
        ctx->features = 0;
        memset(ctx->ranges, 0, sizeof(ctx->ranges));
        memset(ctx->events, 0, sizeof(ctx->events));
    }
    uffd_initialised = 1;
    kprintf("[OK] uffd: initialised with %d contexts (range tracking + SIGBUS)\n",
            UFFD_MAX_CONTEXTS);
}

static int uffd_find_free(void)
{
    for (int i = 0; i < UFFD_MAX_CONTEXTS; i++) {
        if (!uffd_table[i].used)
            return i;
    }
    return -EMFILE;
}

/* ── Range management helpers ─────────────────────────────────────────── */

/* Check if [a_start, a_end) overlaps with [b_start, b_end) */
static int ranges_overlap(uint64_t a_start, uint64_t a_end,
                          uint64_t b_start, uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

/* Add or extend a range in the context.  Ranges are kept non-overlapping:
 * if the new range overlaps existing ones, it is merged. */
static int range_add(struct uffd_context *ctx, uint64_t start, uint64_t end,
                     int mode)
{
    /* Merge with any existing overlapping ranges */
    uint64_t merged_start = start;
    uint64_t merged_end = end;
    int merged_mode = mode;

    for (int i = 0; i < ctx->num_ranges; i++) {
        struct uffd_range *r = &ctx->ranges[i];
        if (!r->in_use)
            continue;
        if (ranges_overlap(merged_start, merged_end, r->start, r->end)) {
            /* Extend the merged range to cover the existing one */
            if (r->start < merged_start)
                merged_start = r->start;
            if (r->end > merged_end)
                merged_end = r->end;
            /* Merge mode flags */
            merged_mode |= r->mode;
            /* Mark existing range as free (will be replaced) */
            r->in_use = 0;
        }
    }

    /* Find a free slot or compact */
    int slot = -1;
    for (int i = 0; i < UFFD_MAX_RANGES; i++) {
        if (!ctx->ranges[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0)
        return -ENOMEM;

    ctx->ranges[slot].start  = merged_start;
    ctx->ranges[slot].end    = merged_end;
    ctx->ranges[slot].mode   = merged_mode;
    ctx->ranges[slot].in_use = 1;

    /* Update count (recount active ranges) */
    ctx->num_ranges = 0;
    for (int i = 0; i < UFFD_MAX_RANGES; i++) {
        if (ctx->ranges[i].in_use)
            ctx->num_ranges++;
    }

    return 0;
}

/* Remove a range exactly matching [start, end).  If the range covers only
 * part of a registered region, the remaining parts are split out. */
static int range_remove(struct uffd_context *ctx, uint64_t start, uint64_t end)
{
    int found = 0;

    for (int i = 0; i < UFFD_MAX_RANGES; i++) {
        struct uffd_range *r = &ctx->ranges[i];
        if (!r->in_use)
            continue;

        /* Check overlap */
        if (!ranges_overlap(start, end, r->start, r->end))
            continue;

        found = 1;

        if (start <= r->start && end >= r->end) {
            /* Removal wholly covers this range — delete entirely */
            r->in_use = 0;
        } else if (start > r->start && end < r->end) {
            /* Removal splits range into two: keep left and right parts */
            uint64_t old_end = r->end;
            /* Shrink current range to [r->start, start) */
            r->end = start;

            /* Add new range [end, old_end) if non-empty */
            if (end < old_end) {
                int slot = -1;
                for (int j = 0; j < UFFD_MAX_RANGES; j++) {
                    if (!ctx->ranges[j].in_use) {
                        slot = j;
                        break;
                    }
                }
                if (slot >= 0) {
                    ctx->ranges[slot].start  = end;
                    ctx->ranges[slot].end    = old_end;
                    ctx->ranges[slot].mode   = r->mode;
                    ctx->ranges[slot].in_use = 1;
                }
            }
        } else if (start <= r->start) {
            /* Removal overlaps the beginning: r->start moves to end */
            r->start = end;
        } else if (end >= r->end) {
            /* Removal overlaps the end: r->end moves to start */
            r->end = start;
        }
    }

    if (!found)
        return -ENOENT;

    /* Recount */
    ctx->num_ranges = 0;
    for (int i = 0; i < UFFD_MAX_RANGES; i++) {
        if (ctx->ranges[i].in_use)
            ctx->num_ranges++;
    }

    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int userfaultfd_create(int flags)
{
    (void)flags;

    if (!uffd_initialised)
        return -ENOSYS;

    int fd = uffd_find_free();
    if (fd < 0)
        return fd;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);
    ctx->used = 1;
    ctx->event_head = 0;
    ctx->event_tail = 0;
    ctx->num_ranges = 0;
    ctx->features = 0;
    memset(ctx->ranges, 0, sizeof(ctx->ranges));
    memset(ctx->events, 0, sizeof(ctx->events));
    spinlock_release(&ctx->lock);

    return fd;
}

int userfaultfd_register(int fd, uint64_t addr, uint64_t len, int mode)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;
    if (addr + len < addr || len == 0)
        return -EINVAL;
    /* Validate mode flags */
    if (mode == 0 || (mode & ~(UFFD_FLAG_MISSING | UFFD_FLAG_WP | UFFD_FLAG_MINOR)))
        return -EINVAL;

    /* Address must be page-aligned, length must be page-granular */
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len & (PAGE_SIZE - 1))
        return -EINVAL;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);

    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }

    uint64_t end = addr + len;
    int ret = range_add(ctx, addr, end, mode);

    spinlock_release(&ctx->lock);
    return ret;
}

int userfaultfd_unregister(int fd, uint64_t addr, uint64_t len)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;
    if (addr + len < addr || len == 0)
        return -EINVAL;

    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len & (PAGE_SIZE - 1))
        return -EINVAL;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);

    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }

    int ret = range_remove(ctx, addr, addr + len);

    spinlock_release(&ctx->lock);
    return ret;
}

int userfaultfd_handle_fault(int fd, uint64_t fault_addr, int write,
                              int is_minor)
{
    if (!uffd_initialised)
        return 1; /* not handled */
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return 1;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);

    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return 1;
    }

    /* Check if the fault address falls within any registered range */
    int range_idx = -1;
    int range_mode = 0;
    for (int i = 0; i < ctx->num_ranges; i++) {
        struct uffd_range *r = &ctx->ranges[i];
        if (!r->in_use)
            continue;
        if (fault_addr >= r->start && fault_addr < r->end) {
            range_idx = i;
            range_mode = r->mode;
            break;
        }
    }

    if (range_idx < 0) {
        /* Address not covered by any uffd range — not handled */
        spinlock_release(&ctx->lock);
        return 1;
    }

    /* Check fault type against registered mode */
    if (is_minor && !(range_mode & UFFD_FLAG_MINOR)) {
        spinlock_release(&ctx->lock);
        return 1; /* minor fault but range not monitoring minors */
    }
    if (write && !(range_mode & UFFD_FLAG_WP)) {
        /* Write fault but WP not registered — allow it to proceed normally */
        spinlock_release(&ctx->lock);
        return 1;
    }

    /* ── UFFD_FEATURE_SIGBUS: deliver SIGBUS instead of queuing event ── */
    if (ctx->features & UFFD_FEATURE_SIGBUS) {
        spinlock_release(&ctx->lock);

        /* Send SIGBUS to the current process with the fault address */
        struct process *p = process_get_current();
        if (p) {
            struct siginfo info;
            memset(&info, 0, sizeof(info));
            info.si_signo = SIGBUS;
            info.si_errno = 0;
            info.si_code  = SI_KERNEL;
            info.si_addr  = (void *)fault_addr;
            signal_send_info(p->pid, SIGBUS, &info);
        }
        return 0; /* handled */
    }

    /* ── Normal path: queue an event in the ring buffer ── */
    int next_head = (ctx->event_head + 1) % UFFD_EVENT_RING_SIZE;
    if (next_head == ctx->event_tail) {
        /* Ring buffer full — can't queue event */
        spinlock_release(&ctx->lock);
        return -ENOSPC;
    }

    struct uffd_event *ev = &ctx->events[ctx->event_head];
    ev->fault_addr  = fault_addr;
    ev->fault_flags = (write ? 1 : 0) | (is_minor ? 2 : 0);
    ev->pending     = 1;
    ctx->event_head = next_head;

    spinlock_release(&ctx->lock);
    return 0; /* handled (event queued) */
}

int64_t userfaultfd_read(int fd, void *buf, uint64_t count)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;
    if (!buf || count < sizeof(struct uffd_event))
        return -EINVAL;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);

    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }

    if (ctx->event_tail == ctx->event_head) {
        spinlock_release(&ctx->lock);
        return 0;  /* no events available */
    }

    /* Dequeue a single event */
    struct uffd_event *ev = &ctx->events[ctx->event_tail];
    if (count < sizeof(*ev)) {
        spinlock_release(&ctx->lock);
        return -EINVAL;
    }

    memcpy(buf, ev, sizeof(*ev));
    ev->pending = 0;
    ctx->event_tail = (ctx->event_tail + 1) % UFFD_EVENT_RING_SIZE;

    spinlock_release(&ctx->lock);
    return (int64_t)sizeof(struct uffd_event);
}

int userfaultfd_set_features(int fd, uint64_t features)
{
    if (!uffd_initialised)
        return -ENOSYS;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;

    struct uffd_context *ctx = &uffd_table[fd];
    spinlock_acquire(&ctx->lock);

    if (!ctx->used) {
        spinlock_release(&ctx->lock);
        return -EBADF;
    }

    /* Only recognize known feature bits */
    ctx->features = features & UFFD_FEATURE_SIGBUS;

    spinlock_release(&ctx->lock);
    return 0;
}
