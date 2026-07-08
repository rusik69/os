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
#include "printf.h"
#include "errno.h"
#include "process.h"
#include "signal.h"
#include "vmm.h"
#include "pmm.h"
#include "uaccess.h"

static struct uffd_context uffd_table[UFFD_MAX_CONTEXTS];
static int uffd_initialised = 0;

void __init uffd_init(void)
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
        ctx->wake_pending_count = 0;
        memset(ctx->wake_pending_addrs, 0, sizeof(ctx->wake_pending_addrs));
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
        return 0;

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
        return 0;
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
        return 0;
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

    /* Add to the wake-pending list so UFFDIO_COPY / UFFDIO_WAKE
     * can track resolution of this page. */
    if (ctx->wake_pending_count < 16) {
        ctx->wake_pending_addrs[ctx->wake_pending_count++] = fault_addr;
    }

    spinlock_release(&ctx->lock);
    return 0; /* handled (event queued) */
}

int64_t userfaultfd_read(int fd, void *buf, uint64_t count)
{
    if (!uffd_initialised)
        return 0;
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
        return 0;
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

/* ── UFFDIO operations ───────────────────────────────────────────── */

static int uffd_validate_ctx(int fd, struct uffd_context **out_ctx)
{
    if (!uffd_initialised)
        return -ENODEV;
    if (fd < 0 || fd >= UFFD_MAX_CONTEXTS)
        return -EBADF;
    struct uffd_context *ctx = &uffd_table[fd];
    if (!ctx->used)
        return -EBADF;
    if (out_ctx)
        *out_ctx = ctx;
    return 0;
}

/*
 * Find which userfaultfd context (if any) handles a given fault address
 * for the current process.  Scans all uffd contexts for ranges covering
 * 'fault_addr'.  Returns the fd on success, -1 if not found.
 * Sets *write_flag to 1 if the matching range has UFFD_FLAG_WP set.
 */
int userfaultfd_find_for_addr(uint64_t fault_addr, int *write_flag)
{
    if (!uffd_initialised)
        return -1;

    for (int fd = 0; fd < UFFD_MAX_CONTEXTS; fd++) {
        struct uffd_context *ctx = &uffd_table[fd];
        if (!ctx->used)
            continue;

        spinlock_acquire(&ctx->lock);
        for (int i = 0; i < ctx->num_ranges; i++) {
            struct uffd_range *r = &ctx->ranges[i];
            if (!r->in_use)
                continue;
            if (fault_addr >= r->start && fault_addr < r->end) {
                if (write_flag)
                    *write_flag = (r->mode & UFFD_FLAG_WP) ? 1 : 0;
                spinlock_release(&ctx->lock);
                return fd;
            }
        }
        spinlock_release(&ctx->lock);
    }

    return -1; /* not found */
}

/*
 * UFFDIO_API — Create or negotiate API version and features.
 * When 'api' field matches UFFD_API, creates a new context and
 * returns its fd.  Always fills in features and ioctls.
 */
int userfaultfd_api(int fd, struct uffdio_api *api_arg)
{
    if (!api_arg)
        return -EFAULT;

    /* Report supported features and ioctls */
    api_arg->features = UFFD_FEATURE_SIGBUS;
    /* Use ioctl command numbers directly in the bitmask (lowest 6 bits) */
    api_arg->ioctls = (1ULL << (UFFDIO_API & 0x3F)) |
                      (1ULL << (UFFDIO_REGISTER & 0x3F)) |
                      (1ULL << (UFFDIO_UNREGISTER & 0x3F)) |
                      (1ULL << (UFFDIO_COPY & 0x3F)) |
                      (1ULL << (UFFDIO_ZEROPAGE & 0x3F)) |
                      (1ULL << (UFFDIO_WAKE & 0x3F));

    if (api_arg->api != UFFD_API)
        return -EINVAL;

    /* Create a new context if the caller is requesting via UFFDIO_API
     * on fd == -1 (initial creation path).  Otherwise it's a query on an
     * existing fd (which just returns the above). */
    if (fd < 0) {
        int new_fd = userfaultfd_create(0);
        if (new_fd < 0)
            return new_fd;
        return new_fd; /* positive fd on success */
    }

    return 0; /* query on existing fd succeeded */
}

/*
 * UFFDIO_REGISTER — Register a virtual memory range with the uffd context.
 * Arg comes from userspace; we trust the caller already validated it.
 */
static int userfaultfd_register_ioctl(int fd, struct uffdio_register *reg_arg)
{
    if (!reg_arg)
        return -EFAULT;

    int ret = userfaultfd_register(fd, reg_arg->range_start,
                                    reg_arg->range_len,
                                    (int)reg_arg->mode);
    if (ret == 0) {
        /* Report which ioctls are available for this range */
        reg_arg->ioctls = (1ULL << (UFFDIO_WAKE & 0x3F)) |
                          (1ULL << (UFFDIO_COPY & 0x3F)) |
                          (1ULL << (UFFDIO_ZEROPAGE & 0x3F));
    }
    return ret;
}

/*
 * UFFDIO_UNREGISTER — Unregister a virtual memory range.
 */
static int userfaultfd_unregister_ioctl(int fd, struct uffdio_register *unreg_arg)
{
    if (!unreg_arg)
        return -EFAULT;
    return userfaultfd_unregister(fd, unreg_arg->range_start,
                                   unreg_arg->range_len);
}

/*
 * UFFDIO_COPY — Copy a page from userspace into the tracked address range.
 * This resolves the pending page fault by mapping a physical page at 'dst'
 * and copying the contents from 'src'.
 *
 * The operation:
 *   1. Allocate a physical frame
 *   2. Copy data from src (user virtual) into the frame
 *   3. Map the frame at dst with appropriate permissions
 *   4. Optionally wake the waiting thread (unless DONTWAKE flag set)
 */
int userfaultfd_copy(int fd, struct uffdio_copy *copy_arg)
{
    struct uffd_context *ctx = NULL;
    int ret = uffd_validate_ctx(fd, &ctx);
    if (ret < 0)
        return ret;

    if (!copy_arg)
        return -EFAULT;

    /* Validate alignment and length */
    if (copy_arg->dst & (PAGE_SIZE - 1))
        return -EINVAL;
    if (copy_arg->src & (PAGE_SIZE - 1))
        return -EINVAL;
    if (copy_arg->len != PAGE_SIZE)  /* currently we only handle 1 page at a time */
        return -EINVAL;
    if (copy_arg->mode & ~UFFDIO_COPY_MODE_DONTWAKE)
        return -EINVAL;

    /* Validate dst is within a registered range */
    uint64_t end_addr = copy_arg->dst + copy_arg->len;
    int covered = 0;
    spinlock_acquire(&ctx->lock);
    for (int i = 0; i < ctx->num_ranges; i++) {
        struct uffd_range *r = &ctx->ranges[i];
        if (!r->in_use) continue;
        if (copy_arg->dst >= r->start && end_addr <= r->end) {
            covered = 1;
            break;
        }
    }
    spinlock_release(&ctx->lock);

    if (!covered)
        return -ENOENT;

    /* Allocate a physical frame */
    struct process *cur = process_get_current();
    if (!cur || !cur->pml4)
        return -ESRCH;

    uint64_t frame = pmm_alloc_frame();
    if (!frame)
        return -ENOMEM;

    /* Copy data from userspace source */
    uint8_t *kbuf = (uint8_t *)PHYS_TO_VIRT(frame);
    if (copy_from_user(kbuf, copy_arg->src, PAGE_SIZE) < 0) {
        pmm_free_frame(frame);
        return -EFAULT;
    }

    /* Map the page at dst in the current process's page table */
    uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE | VMM_FLAG_NOEXEC;
    ret = vmm_map_user_page(cur->pml4, copy_arg->dst, frame, vmm_flags);
    if (ret < 0) {
        pmm_free_frame(frame);
        return -ENOMEM;
    }

    /* Copy succeeded — report bytes copied */
    copy_arg->copy = PAGE_SIZE;

    /* ── Wake path ──────────────────────────────────────────────────
     * If the caller did NOT set DONTWAKE, this page has been resolved
     * and any thread waiting on it should be woken up.
     *
     * In a synchronous model the fault handler doesn't block threads,
     * but we maintain a pending-address list for correctness so that
     * the wake accounting is accurate.  A future enhancement would
     * actually unblock waiting threads via a per-range waitqueue. */
    if (!(copy_arg->mode & UFFDIO_COPY_MODE_DONTWAKE)) {
        /* Remove this address from the wake-pending list */
        spinlock_acquire(&ctx->lock);
        for (int i = 0; i < ctx->wake_pending_count; i++) {
            if (ctx->wake_pending_addrs[i] == copy_arg->dst) {
                /* Shift remaining entries down */
                for (int j = i; j < ctx->wake_pending_count - 1; j++)
                    ctx->wake_pending_addrs[j] = ctx->wake_pending_addrs[j + 1];
                ctx->wake_pending_count--;
                break;
            }
        }
        spinlock_release(&ctx->lock);
        kprintf("[uffd] wake: pid=%u dst=0x%llx resolved\n",
                (unsigned)cur->pid, copy_arg->dst);
    }

    kprintf("[uffd] copy pid=%u dst=0x%llx src=0x%llx len=%llu\n",
            (unsigned)cur->pid, copy_arg->dst, copy_arg->src, copy_arg->len);

    return 0;
}

/*
 * UFFDIO_ZEROPAGE — Map a zero-filled page at the given address range.
 * Uses the shared zero page to avoid allocating a new frame.
 */
int userfaultfd_zeropage(int fd, struct uffdio_zeropage *zp_arg)
{
    struct uffd_context *ctx = NULL;
    int ret = uffd_validate_ctx(fd, &ctx);
    if (ret < 0)
        return ret;

    if (!zp_arg)
        return -EFAULT;

    if (zp_arg->range_start & (PAGE_SIZE - 1))
        return -EINVAL;
    if (zp_arg->range_len != PAGE_SIZE)
        return -EINVAL;
    if (zp_arg->mode & ~UFFDIO_ZEROPAGE_MODE_DONTWAKE)
        return -EINVAL;

    /* Validate range is within a registered region */
    uint64_t end_addr = zp_arg->range_start + zp_arg->range_len;
    int covered = 0;
    spinlock_acquire(&ctx->lock);
    for (int i = 0; i < ctx->num_ranges; i++) {
        struct uffd_range *r = &ctx->ranges[i];
        if (!r->in_use) continue;
        if (zp_arg->range_start >= r->start && end_addr <= r->end) {
            covered = 1;
            break;
        }
    }
    spinlock_release(&ctx->lock);

    if (!covered)
        return -ENOENT;

    struct process *cur = process_get_current();
    if (!cur || !cur->pml4)
        return -ESRCH;

    /* Map the shared zero page at the fault address */
    uint64_t zpage_frame = vmm_zero_page_frame;
    if (!zpage_frame) {
        /* Fallback: allocate a zeroed page */
        zpage_frame = pmm_alloc_frame();
        if (!zpage_frame)
            return -ENOMEM;
        memset(PHYS_TO_VIRT(zpage_frame), 0, PAGE_SIZE);
        vmm_zero_page_frame = zpage_frame;
    }

    uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE |
                         VMM_FLAG_NOEXEC | VMM_FLAG_COW; /* COW so writes get a private copy */
    ret = vmm_map_user_page(cur->pml4, zp_arg->range_start, zpage_frame, vmm_flags);
    if (ret < 0)
        return -ENOMEM;

    zp_arg->zeropage = PAGE_SIZE;

    kprintf("[uffd] zeropage pid=%u addr=0x%llx\n",
            (unsigned)cur->pid, zp_arg->range_start);

    return 0;
}

/*
 * UFFDIO_WAKE — Wake any threads blocked waiting on a page range.
 *
 * In our synchronous model the fault handler does not block threads,
 * but we still maintain the wake-pending list for correctness.
 * This call scans the pending list and removes entries that fall
 * within the requested range.
 */
int userfaultfd_wake(int fd, struct uffdio_wake *wake_arg)
{
    struct uffd_context *ctx = NULL;
    int ret = uffd_validate_ctx(fd, &ctx);
    if (ret < 0)
        return ret;

    if (!wake_arg)
        return -EFAULT;

    if (wake_arg->range_start & (PAGE_SIZE - 1))
        return -EINVAL;
    if (wake_arg->range_len & (PAGE_SIZE - 1))
        return -EINVAL;

    uint64_t range_end = wake_arg->range_start + wake_arg->range_len;

    spinlock_acquire(&ctx->lock);

    /* Walk the pending list and remove any addresses in the wake range */
    int woken = 0;
    for (int i = ctx->wake_pending_count - 1; i >= 0; i--) {
        uint64_t addr = ctx->wake_pending_addrs[i];
        if (addr >= wake_arg->range_start && addr < range_end) {
            /* Remove by shifting later entries down */
            for (int j = i; j < ctx->wake_pending_count - 1; j++)
                ctx->wake_pending_addrs[j] = ctx->wake_pending_addrs[j + 1];
            ctx->wake_pending_count--;
            woken++;
        }
    }

    spinlock_release(&ctx->lock);

    kprintf("[uffd] wake: range=0x%llx-0x%llx woken=%d\n",
            wake_arg->range_start, range_end, woken);

    return 0;
}

/* ── Syscall entry point ─────────────────────────────────────────── */

/*
 * UFFDIO_WRITEPROTECT — Change the write-protection status on a range.
 *
 * When wp_arg->mode & UFFDIO_WRITEPROTECT_MODE_WP is set, the pages
 * in the range are write-protected so that write faults will trigger
 * a uffd event.  When clear, write protection is removed.
 *
 * In our implementation, we mark the tracked range with UFFD_FLAG_WP
 * (or clear it), and update the actual page table entries in the
 * current process's address space.
 */
int userfaultfd_writeprotect(int fd, struct uffdio_writeprotect *wp_arg)
{
    struct uffd_context *ctx = NULL;
    int ret = uffd_validate_ctx(fd, &ctx);
    if (ret < 0)
        return ret;

    if (!wp_arg)
        return -EFAULT;

    if (wp_arg->range_start & (PAGE_SIZE - 1))
        return -EINVAL;
    if (wp_arg->range_len & (PAGE_SIZE - 1))
        return -EINVAL;
    if (wp_arg->range_len == 0)
        return -EINVAL;

    int wp = (wp_arg->mode & UFFDIO_WRITEPROTECT_MODE_WP) ? 1 : 0;
    uint64_t end = wp_arg->range_start + wp_arg->range_len;

    spinlock_acquire(&ctx->lock);

    /* Update range modes: for each overlapping range, set or clear the WP flag */
    for (int i = 0; i < ctx->num_ranges; i++) {
        struct uffd_range *r = &ctx->ranges[i];
        if (!r->in_use)
            continue;
        if (ranges_overlap(wp_arg->range_start, end, r->start, r->end)) {
            if (wp)
                r->mode |= UFFD_FLAG_WP;
            else
                r->mode &= ~UFFD_FLAG_WP;
        }
    }

    spinlock_release(&ctx->lock);

    /* Update page table entries in the current process */
    struct process *cur = process_get_current();
    if (cur && cur->pml4) {
        uint64_t num_pages = wp_arg->range_len / PAGE_SIZE;
        if (num_pages > 0) {
            if (wp) {
                /* Remove the writable flag from the PTEs */
                vmm_set_user_pages_flags(cur->pml4, wp_arg->range_start,
                                         num_pages,
                                         VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_NOEXEC);
            } else {
                /* Add the writable flag back */
                vmm_set_user_pages_flags(cur->pml4, wp_arg->range_start,
                                         num_pages,
                                         VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE | VMM_FLAG_NOEXEC);
            }
        }
    }

    wp_arg->result = (int64_t)wp_arg->range_len;

    kprintf("[uffd] writeprotect pid=%u range=0x%llx-0x%llx wp=%d\n",
            (unsigned)(cur ? cur->pid : 0),
            wp_arg->range_start, end, wp);

    return 0;
}

/*
 * UFFDIO_CONTINUE — Continue execution after a minor page fault.
 *
 * Called after the page-cache page has been resolved (e.g., by backing
 * it with a physical page).  This tells the uffd handler that the fault
 * has been handled and the faulting thread can resume.
 *
 * In our synchronous model, we mark the event as resolved via the
 * wake-pending tracker and ensure the page is mapped.
 */
int userfaultfd_continue(int fd, struct uffdio_continue *cont_arg)
{
    struct uffd_context *ctx = NULL;
    int ret = uffd_validate_ctx(fd, &ctx);
    if (ret < 0)
        return ret;

    if (!cont_arg)
        return -EFAULT;

    if (cont_arg->range_start & (PAGE_SIZE - 1))
        return -EINVAL;
    if (cont_arg->range_len & (PAGE_SIZE - 1))
        return -EINVAL;
    if (cont_arg->range_len != PAGE_SIZE)
        return -EINVAL;

    uint64_t end = cont_arg->range_start + cont_arg->range_len;

    /* Validate the range is within a registered MINOR range */
    int covered = 0;
    spinlock_acquire(&ctx->lock);
    for (int i = 0; i < ctx->num_ranges; i++) {
        struct uffd_range *r = &ctx->ranges[i];
        if (!r->in_use) continue;
        if (!(r->mode & UFFD_FLAG_MINOR)) continue;
        if (cont_arg->range_start >= r->start && end <= r->end) {
            covered = 1;
            break;
        }
    }
    spinlock_release(&ctx->lock);

    if (!covered)
        return -ENOENT;

    /* Remove from wake-pending list if not DONTWAKE */
    if (!(cont_arg->mode & UFFDIO_CONTINUE_MODE_DONTWAKE)) {
        spinlock_acquire(&ctx->lock);
        for (int i = 0; i < ctx->wake_pending_count; i++) {
            if (ctx->wake_pending_addrs[i] == cont_arg->range_start) {
                for (int j = i; j < ctx->wake_pending_count - 1; j++)
                    ctx->wake_pending_addrs[j] = ctx->wake_pending_addrs[j + 1];
                ctx->wake_pending_count--;
                break;
            }
        }
        spinlock_release(&ctx->lock);
    }

    cont_arg->result = (int64_t)cont_arg->range_len;

    struct process *cur = process_get_current();
    kprintf("[uffd] continue pid=%u addr=0x%llx len=%llu\n",
            (unsigned)(cur ? cur->pid : 0),
            cont_arg->range_start, cont_arg->range_len);

    return 0;
}

/*
 * sys_userfaultfd2 — Unified userfaultfd syscall entry point.
 *
 * Called from syscall dispatch with:
 *   a1=fd (userfaultfd context fd)
 *   a2=cmd (UFFDIO_* command)
 *   a3=arg_ptr (userspace pointer to command-specific struct)
 *
 * Returns fd (for UFFDIO_API create), 0 on success, or negative errno.
 */
int64_t __no_sanitize_address sys_userfaultfd2(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    struct process *cur = process_get_current();
    if (!cur || !cur->is_user || !cur->pml4)
        return (int64_t)-ESRCH;

    switch (cmd) {
    case UFFDIO_API: {
        /* Create new context or query */
        struct uffdio_api api;
        if (!arg) {
            /* If arg is NULL, just create without negotiate */
            int new_fd = userfaultfd_create(0);
            return (int64_t)new_fd;
        }
        if (copy_from_user(&api, arg, sizeof(api)) < 0)
            return (int64_t)-EFAULT;

        if (fd == (uint64_t)-1) {
            /* Create new context */
            int ret = userfaultfd_api(-1, &api);
            if (ret < 0)
                return (int64_t)ret;
            if (copy_to_user(arg, &api, sizeof(api)) < 0)
                return (int64_t)-EFAULT;
            return (int64_t)ret;
        } else {
            /* Query existing fd */
            int ret = userfaultfd_api((int)fd, &api);
            if (ret < 0)
                return (int64_t)ret;
            if (copy_to_user(arg, &api, sizeof(api)) < 0)
                return (int64_t)-EFAULT;
            return 0;
        }
    }

    case UFFDIO_REGISTER: {
        struct uffdio_register reg;
        if (copy_from_user(&reg, arg, sizeof(reg)) < 0)
            return (int64_t)-EFAULT;
        int ret = userfaultfd_register_ioctl((int)fd, &reg);
        if (ret < 0)
            return (int64_t)ret;
        if (copy_to_user(arg, &reg, sizeof(reg)) < 0)
            return (int64_t)-EFAULT;
        return 0;
    }

    case UFFDIO_UNREGISTER: {
        struct uffdio_register unreg;
        if (copy_from_user(&unreg, arg, sizeof(unreg)) < 0)
            return (int64_t)-EFAULT;
        int ret = userfaultfd_unregister_ioctl((int)fd, &unreg);
        if (ret < 0)
            return (int64_t)ret;
        return 0;
    }

    case UFFDIO_COPY: {
        struct uffdio_copy copy_arg;
        if (copy_from_user(&copy_arg, arg, sizeof(copy_arg)) < 0)
            return (int64_t)-EFAULT;
        int ret = userfaultfd_copy((int)fd, &copy_arg);
        if (ret < 0)
            return (int64_t)ret;
        if (copy_to_user(arg, &copy_arg, sizeof(copy_arg)) < 0)
            return (int64_t)-EFAULT;
        return 0;
    }

    case UFFDIO_ZEROPAGE: {
        struct uffdio_zeropage zp;
        if (copy_from_user(&zp, arg, sizeof(zp)) < 0)
            return (int64_t)-EFAULT;
        int ret = userfaultfd_zeropage((int)fd, &zp);
        if (ret < 0)
            return (int64_t)ret;
        if (copy_to_user(arg, &zp, sizeof(zp)) < 0)
            return (int64_t)-EFAULT;
        return 0;
    }

    case UFFDIO_WAKE: {
        struct uffdio_wake wake_arg;
        if (copy_from_user(&wake_arg, arg, sizeof(wake_arg)) < 0)
            return (int64_t)-EFAULT;
        int ret = userfaultfd_wake((int)fd, &wake_arg);
        if (ret < 0)
            return (int64_t)ret;
        return 0;
    }

    case UFFDIO_WRITEPROTECT: {
        struct uffdio_writeprotect wp_arg;
        if (copy_from_user(&wp_arg, arg, sizeof(wp_arg)) < 0)
            return (int64_t)-EFAULT;
        int ret = userfaultfd_writeprotect((int)fd, &wp_arg);
        if (ret < 0)
            return (int64_t)ret;
        if (copy_to_user(arg, &wp_arg, sizeof(wp_arg)) < 0)
            return (int64_t)-EFAULT;
        return 0;
    }

    case UFFDIO_CONTINUE: {
        struct uffdio_continue cont_arg;
        if (copy_from_user(&cont_arg, arg, sizeof(cont_arg)) < 0)
            return (int64_t)-EFAULT;
        int ret = userfaultfd_continue((int)fd, &cont_arg);
        if (ret < 0)
            return (int64_t)ret;
        if (copy_to_user(arg, &cont_arg, sizeof(cont_arg)) < 0)
            return (int64_t)-EFAULT;
        return 0;
    }

    default:
        return (int64_t)-ENOSYS;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Stub functions for incomplete userfaultfd operations
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Stub: userfaultfd_poison ──────────────────────────────────────────── */
static int userfaultfd_poison(int fd, uint64_t addr, uint64_t len, uint64_t mode)
{
    (void)fd;
    (void)addr;
    (void)len;
    (void)mode;
    kprintf("[uffd] userfaultfd_poison not yet implemented\n");
    return 0;
}

/* ── Stub: userfaultfd_remap ───────────────────────────────────────────── */
static int userfaultfd_remap(int fd, uint64_t addr, uint64_t len, uint64_t new_addr)
{
    (void)fd;
    (void)addr;
    (void)len;
    (void)new_addr;
    kprintf("[uffd] userfaultfd_remap not yet implemented\n");
    return 0;
}

/* ── Stub: userfaultfd_unmap ───────────────────────────────────────────── */
static int userfaultfd_unmap(int fd, uint64_t addr, uint64_t len)
{
    (void)fd;
    (void)addr;
    (void)len;
    kprintf("[uffd] userfaultfd_unmap not yet implemented\n");
    return 0;
}
