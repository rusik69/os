#ifndef IOSCHED_H
#define IOSCHED_H

#include "types.h"
#include "blockdev.h"
#include "list.h"

/*
 * I/O Scheduler (Elevator) — B15
 *
 * Pluggable I/O scheduling layer that sits above the block device
 * drivers.  Supports three policies:
 *   NOOP     — simple FIFO (suitable for SSDs / NVMe)
 *   DEADLINE — read-biased with per-request deadlines
 *   CFQ      — Complete Fair Queueing with per-process queues
 */

/* Scheduler policy identifiers */
#define IOSCHED_NOOP     0
#define IOSCHED_DEADLINE 1
#define IOSCHED_CFQ      2

/* Deadline constants */
#define DEADLINE_READ_MS   500   /* read deadline in milliseconds */
#define DEADLINE_WRITE_MS 5000   /* write deadline in milliseconds */

/* CFQ constants */
#define CFQ_SLICE_MS      100    /* time slice per process queue (ms) */
#define CFQ_QUEUES_MAX     16    /* max per-process queues */

/* Forward declaration */
struct iosched_queue;
struct iosched_cfq_data;

/* ── Per-scheduler operations ────────────────────────────────────── */

struct iosched_ops {
    const char *name;
    int  (*submit)(struct iosched_queue *iq, struct blk_request *req);
    struct blk_request *(*fetch)(struct iosched_queue *iq);
    void (*free)(struct iosched_queue *iq);
};

/* ── Deadline scheduler data ─────────────────────────────────────── */

#define DD_READ_QUEUE  0
#define DD_WRITE_QUEUE 1
#define DD_QUEUE_COUNT 2

struct dd_per_queue {
    struct blk_request *head;
    struct blk_request *tail;
    int                 count;
};

struct iosched_deadline_data {
    struct dd_per_queue queues[DD_QUEUE_COUNT];   /* read / write fifo queues */
    struct blk_request *fifo_list[DD_QUEUE_COUNT]; /* deadline-sorted fifo */
    struct blk_request *fifo_tail[DD_QUEUE_COUNT];
    int                 fifo_count[DD_QUEUE_COUNT];
    int                 current_queue;             /* read or write batch */
    uint64_t            last_tick;                 /* last batch switch time */
    int                 starved;                   /* write starvation counter */
};

/* ── CFQ scheduler data ──────────────────────────────────────────── */

struct cfq_queue {
    struct list_head      list;        /* link in cfq_data->active_queues */
    struct blk_request   *head;
    struct blk_request   *tail;
    int                   count;
    uint64_t              pid;         /* owning process PID */
    uint64_t              slice_start; /* tick when slice started */
    int                   dispatched;  /* requests dispatched in current slice */
};

struct iosched_cfq_data {
    struct cfq_queue  queues[CFQ_QUEUES_MAX];
    int               queue_count;
    struct list_head  active_queues;  /* queues with pending requests */
    struct cfq_queue *current_q;      /* queue currently being served */
    int               current_index;  /* index for round-robin */
};

/* ── Per-device I/O scheduler queue ───────────────────────────────── */

struct iosched_queue {
    spinlock_t          lock;
    int                 dev_id;
    int                 policy;          /* IOSCHED_NOOP, _DEADLINE, _CFQ */
    const struct iosched_ops *ops;

    /* Deadline / NOOP: simple linked list of pending requests */
    struct blk_request *head;
    struct blk_request *tail;
    int                 queued_count;

    /* Policy-specific private data */
    union {
        struct iosched_deadline_data deadline;
        struct iosched_cfq_data     cfq;
    };
};

/* ── Public API ──────────────────────────────────────────────────── */

/* Initialise the I/O scheduler subsystem */
void iosched_init(void);

/* Submit a request to the scheduler for the given device */
int  iosched_submit_request(int dev_id, struct blk_request *req);

/* Fetch the next request to dispatch to the driver */
struct blk_request *iosched_fetch_request(int dev_id);

/* Change scheduling policy for a device */
int  iosched_set_policy(int dev_id, int policy);

/* Get current policy */
int  iosched_get_policy(int dev_id);

/* Called when a request completes — used by the block layer */
void iosched_request_complete(int dev_id, struct blk_request *req);

/* Get the iosched_queue for a device (internal) */
struct iosched_queue *iosched_get_queue(int dev_id);

#endif /* IOSCHED_H */
