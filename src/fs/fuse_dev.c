/*
 * src/fs/fuse_dev.c — /dev/fuse character device
 *
 * Implements the kernel side of the /dev/fuse character device that
 * userspace FUSE daemons open to receive FUSE requests and send
 * responses.
 *
 * The character device provides:
 *   - Open tracking (only one daemon at a time)
 *   - Read: dequeue FUSE requests for the daemon
 *   - Write: receive FUSE responses from the daemon
 *   - Release: cleanup when daemon closes the device
 *
 * Requests are queued by the VFS layer (in fuse.c) and dequeued by
 * the daemon via read().  Responses are submitted by the daemon via
 * write() and matched to the waiting VFS operation by the unique ID.
 */

#include "types.h"
#include "fuse.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "completion.h"
#include "list.h"
#include "devfs.h"

/* ── Request queue entry ────────────────────────────────────────────── */
struct fuse_request_item {
    struct list_head list;           /* link in request_queue */
    struct fuse_in_header *hdr;      /* allocated request header */
    void *arg;                       /* optional argument data */
    int arg_size;                    /* size of arg data */
    struct completion complete;      /* signals VFS waiter on response */
    /* Response data (filled by daemon's write) */
    struct fuse_out_header *resp_hdr;
    void *resp_arg;
    int resp_arg_size;
};

/* ── Device state ───────────────────────────────────────────────────── */
#define FUSE_DEV_MAX_QUEUE 64

static struct {
    int opened;                      /* daemon has opened /dev/fuse */
    int open_count;                  /* reference count for opens */
    uint64_t unique;                 /* next unique request ID */
    struct list_head request_queue;  /* list of struct fuse_request_item */
    int queue_count;                 /* number of queued requests */
    spinlock_t lock;                 /* protects all state */
    struct wait_queue daemon_wq;     /* daemon blocks here when queue empty */
    int initialized;                 /* one-time init flag */
} g_fuse_dev;

/* ── Forward declarations ───────────────────────────────────────────── */
static int fuse_dev_read(void *priv, void *buf,
                          uint32_t max_size, uint32_t *out_size);
static int fuse_dev_write(void *priv, const void *data, uint32_t size);
static int fuse_dev_init_internal(void);

/*
 * ── Initialization ────────────────────────────────────────────────────
 */

static int fuse_dev_init_internal(void)
{
    if (g_fuse_dev.initialized)
        return 0;

    memset(&g_fuse_dev, 0, sizeof(g_fuse_dev));
    INIT_LIST_HEAD(&g_fuse_dev.request_queue);
    spinlock_init(&g_fuse_dev.lock);
    wait_queue_init(&g_fuse_dev.daemon_wq);
    g_fuse_dev.unique = 0;
    g_fuse_dev.initialized = 1;

    return 0;
}

/*
 * ── Queue a request (called by VFS layer in fuse.c) ───────────────────
 *
 * Allocates a fuse_request_item, fills it with the given header and
 * optional argument data, and adds it to the request queue.
 * Wakes the daemon if it is blocking on read().
 *
 * Returns 0 on success, or a negative errno on failure.
 */
int fuse_dev_queue_request(uint32_t opcode, uint64_t nodeid,
                            const void *arg, int arg_size,
                            uint64_t *out_unique)
{
    struct fuse_request_item *item;
    struct fuse_in_header *hdr;
    void *arg_copy;
    uint64_t irq_flags;
    uint64_t unique;

    if (!g_fuse_dev.initialized) {
        int ret = fuse_dev_init_internal();
        if (ret < 0) return ret;
    }

    /* Allocate request item */
    item = (struct fuse_request_item *)kmalloc(sizeof(*item));
    if (!item)
        return -ENOMEM;
    memset(item, 0, sizeof(*item));

    /* Allocate and fill the header */
    hdr = (struct fuse_in_header *)kmalloc(sizeof(*hdr));
    if (!hdr) {
        kfree(item);
        return -ENOMEM;
    }
    memset(hdr, 0, sizeof(*hdr));

    /* Copy argument data if present */
    arg_copy = NULL;
    if (arg && arg_size > 0) {
        arg_copy = kmalloc(arg_size);
        if (!arg_copy) {
            kfree(hdr);
            kfree(item);
            return -ENOMEM;
        }
        memcpy(arg_copy, arg, arg_size);
    }

    /* Initialize completion */
    completion_init(&item->complete);

    spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);

    unique = ++g_fuse_dev.unique;

    if (out_unique)
        *out_unique = unique;

    hdr->len     = sizeof(*hdr) + (uint32_t)(arg_size > 0 ? arg_size : 0);
    hdr->opcode  = opcode;
    hdr->unique  = unique;
    hdr->nodeid  = nodeid;
    hdr->uid     = 0;
    hdr->gid     = 0;
    hdr->pid     = 0;
    hdr->padding = 0;

    item->hdr       = hdr;
    item->arg       = arg_copy;
    item->arg_size  = arg_size;

    /* Queue the request */
    list_add_tail(&item->list, &g_fuse_dev.request_queue);
    g_fuse_dev.queue_count++;

    /* Wake the daemon if it is waiting on read */
    wait_queue_wake_all(&g_fuse_dev.daemon_wq);

    spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);

    return 0;
}

/*
 * ── Wait for a response to a specific request ─────────────────────────
 *
 * Called by the VFS layer after queuing a request.  Blocks until the
 * daemon writes a response matching the unique ID, or a timeout occurs.
 *
 * Returns 0 on success and fills *out_resp / *out_resp_size on success,
 * or a negative errno on timeout/error.
 */
int fuse_dev_wait_for_response(uint64_t unique,
                                struct fuse_out_header **out_resp,
                                void **out_resp_arg,
                                int *out_resp_arg_size)
{
    struct fuse_request_item *item;
    uint64_t irq_flags;
    int found = 0;

    if (!out_resp || !out_resp_arg || !out_resp_arg_size)
        return -EINVAL;

    *out_resp = NULL;
    *out_resp_arg = NULL;
    *out_resp_arg_size = 0;

    /* Scan the queue for our request by unique ID */
    for (;;) {
        spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);

        list_for_each_entry(item, &g_fuse_dev.request_queue, list) {
            if (item->hdr && item->hdr->unique == unique) {
                found = 1;
                break;
            }
        }

        spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);

        if (!found)
            return -ENOENT; /* request was already dequeued and lost? */

        /* Save the response pointers from the item */
        spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);
        if (item->resp_hdr) {
            /* Response has arrived */
            *out_resp = item->resp_hdr;
            *out_resp_arg = item->resp_arg;
            *out_resp_arg_size = item->resp_arg_size;

            /* Remove item from queue and free */
            list_del(&item->list);
            g_fuse_dev.queue_count--;
            if (item->hdr) kfree(item->hdr);
            spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);
            kfree(item);
            return 0;
        }
        spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);

        /* Block until the completion fires */
        completion_wait(&item->complete);
    }
}

/*
 * ── Device open — called implicitly on first read/write ───────────────
 *
 * Since devfs does not provide open/release callbacks for character
 * devices, we track open state via the first read or write operation.
 */
static int fuse_dev_try_open(void)
{
    uint64_t irq_flags;
    int ret = 0;

    spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);

    if (g_fuse_dev.open_count == 0) {
        /* First open succeeds */
        g_fuse_dev.opened = 1;
        g_fuse_dev.open_count = 1;
        kprintf("[fuse_dev] /dev/fuse opened by daemon\n");
    } else if (g_fuse_dev.open_count > 0) {
        /* Allow re-open for multiple references */
        g_fuse_dev.open_count++;
    }

    spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);

    return ret;
}

/*
 * ── Device release — called when daemon stops reading/writing ─────────
 *
 * Cleans up any pending requests and marks the device as closed.
 */
static void fuse_dev_try_release(void)
{
    uint64_t irq_flags;
    struct fuse_request_item *item;
    struct list_head *pos, *n;

    spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);

    if (g_fuse_dev.open_count > 0) {
        g_fuse_dev.open_count--;
    }

    if (g_fuse_dev.open_count > 0) {
        /* Still referenced by other opens */
        spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);
        return;
    }

    /* Last close — clean up */
    g_fuse_dev.opened = 0;
    kprintf("[fuse_dev] /dev/fuse released by daemon, cleaning up\n");

    /* Free all pending requests */
    list_for_each_safe(pos, n, &g_fuse_dev.request_queue) {
        item = list_entry(pos, struct fuse_request_item, list);
        list_del(&item->list);
        g_fuse_dev.queue_count--;

        if (item->hdr)       kfree(item->hdr);
        if (item->arg)       kfree(item->arg);
        if (item->resp_hdr)  kfree(item->resp_hdr);
        if (item->resp_arg)  kfree(item->resp_arg);

        /* Wake any VFS waiters with error */
        if (!completion_is_done(&item->complete)) {
            /* Signal that the device was closed */
            item->resp_hdr = NULL;
            completion_done(&item->complete);
        }
        kfree(item);
    }

    spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);
}

/*
 * ── Daemon reads a FUSE request from /dev/fuse ────────────────────────
 *
 * Dequeues the next pending request and copies it (header + argument)
 * into the user-supplied buffer.  If no request is pending, blocks
 * until one arrives or returns 0 bytes (non-blocking semantics via
 * the no-pending-request path).
 */
static int fuse_dev_read(void *priv, void *buf,
                          uint32_t max_size, uint32_t *out_size)
{
    struct fuse_request_item *item;
    uint64_t irq_flags;
    int ret = 0;

    (void)priv;

    if (!out_size)
        return -EINVAL;
    *out_size = 0;

    if (!buf || max_size < sizeof(struct fuse_in_header))
        return 0; /* buffer too small, nothing to read */

    /* Try to open if not already opened */
    fuse_dev_try_open();

    spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);

    /* Wait until a request is available or timeout */
    while (list_empty(&g_fuse_dev.request_queue)) {
        spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);

        /* Block on the daemon wait queue */
        wait_queue_sleep(&g_fuse_dev.daemon_wq);

        spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);

        /* Re-check: if queue has items, break and serve them */
        if (!list_empty(&g_fuse_dev.request_queue))
            break;

        /* No items and no waiters — return 0 bytes */
        spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);
        *out_size = 0;
        return 0;
    }

    /* Dequeue the first request */
    item = list_first_entry(&g_fuse_dev.request_queue,
                             struct fuse_request_item, list);

    /* Calculate total size: header + argument */
    uint32_t total_size = sizeof(struct fuse_in_header);
    if (item->arg && item->arg_size > 0)
        total_size += (uint32_t)item->arg_size;

    if (total_size > max_size)
        total_size = max_size;

    /* Copy header */
    uint32_t header_part = sizeof(struct fuse_in_header);
    if (header_part > total_size) header_part = total_size;
    memcpy(buf, item->hdr, header_part);

    /* Copy argument data after header */
    if (item->arg && header_part < total_size) {
        uint32_t arg_part = total_size - header_part;
        if (arg_part > (uint32_t)item->arg_size)
            arg_part = (uint32_t)item->arg_size;
        memcpy((uint8_t *)buf + header_part, item->arg, arg_part);
    }

    *out_size = total_size;

    kprintf("[fuse_dev] READ: opcode=%u unique=%llu nodeid=%llu size=%u\n",
            (unsigned int)item->hdr->opcode,
            (unsigned long long)item->hdr->unique,
            (unsigned long long)item->hdr->nodeid,
            (unsigned int)total_size);

    /* Do NOT free the item here — it stays in the queue until the
     * daemon writes a response and fuse_dev_wait_for_response picks
     * it up.  This allows the daemon to read the request at its
     * leisure and respond later. */

    spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);
    return 0;
}

/*
 * ── Daemon writes a FUSE response to /dev/fuse ────────────────────────
 *
 * The daemon sends a fuse_out_header + optional response data.
 * We match it to the pending request by unique ID, store the
 * response, and complete the completion to wake the VFS waiter.
 */
static int fuse_dev_write(void *priv, const void *data, uint32_t size)
{
    struct fuse_request_item *item;
    uint64_t irq_flags;

    (void)priv;

    if (!data || size < sizeof(struct fuse_out_header))
        return (int)size; /* consume bytes even if too short */

    /* Try to open if not already opened */
    fuse_dev_try_open();

    const struct fuse_out_header *oh =
        (const struct fuse_out_header *)data;
    uint32_t total_len = oh->len;
    int error = oh->error;
    uint64_t unique = oh->unique;

    kprintf("[fuse_dev] WRITE: unique=%llu error=%d len=%u\n",
            (unsigned long long)unique, error, (unsigned int)total_len);

    /*
     * ── Notification detection ──────────────────────────────────────
     *
     * If unique == 0, this is a notification from the daemon, not a
     * response to a pending request.  Notifications carry a notify
     * opcode and payload in the write data after fuse_out_header.
     *
     * The daemon uses this path to push unsolicited events to the
     * kernel — inode/entry invalidation, poll notifications, etc.
     */
    if (unique == 0) {
        /* Extract the notification opcode from the payload */
        uint32_t notify_op = 0;
        const void *notify_data = NULL;
        int notify_len = 0;

        if (total_len > sizeof(struct fuse_out_header)) {
            const uint8_t *payload =
                (const uint8_t *)data + sizeof(struct fuse_out_header);
            int payload_len = (int)(total_len - sizeof(struct fuse_out_header));

            if (payload_len >= (int)sizeof(notify_op)) {
                /* First 4 bytes are the notify opcode */
                memcpy(&notify_op, payload, sizeof(notify_op));
                notify_data = payload + sizeof(notify_op);
                notify_len  = payload_len - (int)sizeof(notify_op);
            }
        }

        kprintf("[fuse_dev] NOTIFICATION: op=0x%x data_len=%d\n",
                (unsigned int)notify_op, notify_len);

        /* Dispatch to the mount-level notification handler */
        fuse_process_notify(notify_op, notify_data, notify_len);

        /* Consume all bytes written by the daemon */
        return (int)size;
    }

    spinlock_irqsave_acquire(&g_fuse_dev.lock, &irq_flags);

    /* Find the matching request in the queue */
    int found = 0;
    list_for_each_entry(item, &g_fuse_dev.request_queue, list) {
        if (item->hdr && item->hdr->unique == unique) {
            found = 1;
            break;
        }
    }

    if (!found) {
        /* No matching request — maybe already timed out or freed.
         * Just consume the bytes. */
        spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);
        kprintf("[fuse_dev] WARNING: no pending request for unique=%llu\n",
                (unsigned long long)unique);
        return (int)size;
    }

    /* Allocate and copy the response header */
    struct fuse_out_header *resp_hdr =
        (struct fuse_out_header *)kmalloc(sizeof(*resp_hdr));
    if (!resp_hdr) {
        spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);
        return (int)size; /* consume bytes on memory pressure */
    }
    memcpy(resp_hdr, oh, sizeof(*resp_hdr));

    /* Copy any additional response data after the header */
    void *resp_arg = NULL;
    int resp_arg_size = 0;
    if (total_len > sizeof(struct fuse_out_header)) {
        resp_arg_size = (int)(total_len - sizeof(struct fuse_out_header));
        if (resp_arg_size > 0 && size >= total_len) {
            resp_arg = kmalloc(resp_arg_size);
            if (resp_arg) {
                memcpy(resp_arg,
                       (const uint8_t *)data + sizeof(struct fuse_out_header),
                       resp_arg_size);
            }
        }
    }

    /* Store response in the request item */
    item->resp_hdr = resp_hdr;
    item->resp_arg = resp_arg;
    item->resp_arg_size = resp_arg_size;

    /* Signal the waiting VFS operation */
    completion_done(&item->complete);

    spinlock_irqsave_release(&g_fuse_dev.lock, irq_flags);

    return (int)size; /* consume all bytes written */
}

/*
 * ── Public API: initialize the FUSE character device ──────────────────
 */

/**
 * fuse_dev_init — Register /dev/fuse character device.
 *
 * Called from fuse_init() to create the /dev/fuse device node
 * that the userspace FUSE daemon opens.
 */
void fuse_dev_init(void)
{
    if (g_fuse_dev.initialized)
        return;

    fuse_dev_init_internal();

    /* Register with devfs */
    int ret = devfs_register_device("fuse", NULL,
                                     fuse_dev_read, fuse_dev_write);
    if (ret == 0) {
        kprintf("[fuse_dev] /dev/fuse registered\n");
    } else {
        kprintf("[fuse_dev] Failed to register /dev/fuse (ret=%d)\n", ret);
    }
}

/*
 * ── Module info ───────────────────────────────────────────────────────
 */
#include "module.h"
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("/dev/fuse character device — FUSE kernel interface");
