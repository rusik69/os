#ifndef IOSCHED_H
#define IOSCHED_H

#include "types.h"
#include "blockdev.h"
#include "list.h"
#include "hrtimer.h"

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
#define IOSCHED_KYBER    3
#define IOSCHED_BFQ      4

/* ── Kyber scheduler constants ─────────────────────────────────────── */

#define KYBER_MAX_DOMAINS         4
#define KYBER_READ_DOMAIN         0
#define KYBER_WRITE_DOMAIN        1
#define KYBER_DISCARD_DOMAIN      2
#define KYBER_OTHER_DOMAIN        3

/* Default token limits */
#define KYBER_READ_TOKENS_MIN     128
#define KYBER_READ_TOKENS_MAX     512
#define KYBER_WRITE_TOKENS_MIN    16
#define KYBER_WRITE_TOKENS_MAX    128
#define KYBER_DISCARD_TOKENS_MIN  1
#define KYBER_DISCARD_TOKENS_MAX  32
#define KYBER_OTHER_TOKENS_MIN    8
#define KYBER_OTHER_TOKENS_MAX    64

/* Target latencies (ns) */
#define KYBER_READ_TARGET_NS      2000000ULL    /* 2ms */
#define KYBER_WRITE_TARGET_NS     10000000ULL   /* 10ms */
#define KYBER_DISCARD_TARGET_NS   40000000ULL   /* 40ms */
#define KYBER_OTHER_TARGET_NS     20000000ULL   /* 20ms */

/* Token adjustment */
#define KYBER_TOKEN_INTERVAL_NS   1000000ULL    /* 1ms between adjustments */
#define KYBER_TOKEN_ADJUST_STEP   1              /* tokens to adjust per interval */

/* Deadline constants */
#define DEADLINE_READ_MS      500   /* read deadline in milliseconds */
#define DEADLINE_WRITE_MS    5000   /* write deadline in milliseconds */
#define DEADLINE_FIFO_BATCH    16   /* requests per direction batch */
#define DEADLINE_STARVE_LIMIT   2   /* read-batch count before writes dispatched */

/* CFQ constants */
#define CFQ_SLICE_MS         100    /* base time slice per process queue (ms) */
#define CFQ_QUEUES_MAX        16    /* max per-process queues */
#define CFQ_IDLE_DELAY_NS  8000000ULL  /* 8ms idle delay in ns for anticipatory scheduling */
#define CFQ_WRITE_STARVE_NS  2000000000ULL /* 2s write starvation threshold in ns */

/* CFQ priority class weights (relative slice length multiplier) */
#define CFQ_WEIGHT_RT        400   /* RT gets 4x default slice */
#define CFQ_WEIGHT_BE_DEF    100   /* BE default weight */
#define CFQ_WEIGHT_IDLE        1   /* IDLE gets minimum slice */

/* BFQ (Budget Fair Queuing) constants */
#define BFQ_QUEUES_MAX           16    /* max per-process queues */
#define BFQ_DEFAULT_BUDGET       128   /* default budget in sectors (64KB) */
#define BFQ_BUDGET_MIN           8     /* minimum budget */
#define BFQ_BUDGET_MAX           1024  /* maximum budget */
#define BFQ_WEIGHT_RT            400   /* RT gets 4x default weight */
#define BFQ_WEIGHT_BE_DEF        100   /* BE default weight */
#define BFQ_WEIGHT_IDLE          1     /* IDLE gets minimum weight */
#define BFQ_BUDGET_WEIGHT_SCALE  8     /* budget = default * weight / weight_scale */
#define BFQ_SLICE_IDLE_NS        8000000ULL  /* idle timeout for anticipatory (ns) */
#define BFQ_WRITE_STARVE_NS      2000000000ULL /* 2s write starvation threshold (ns) */
#define BFQ_SMALL_REQ_THRESH     4     /* sectors threshold for "small" I/O detection */

/* Forward declarations */
struct iosched_queue;
struct iosched_cfq_data;

/* ── NOOP scheduler data ──────────────────────────────────────────── */

struct iosched_noop_data {
    uint64_t submitted;     /* total requests submitted (excluding merges) */
    uint64_t fetched;       /* total requests fetched */
    uint64_t front_merges;  /* front merges performed */
    uint64_t back_merges;   /* back merges performed */
    uint64_t total_merges;  /* total merges performed (front + back) */
};

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
    int                 starved;                   /* read-batches since last write dispatch */
    int                 batches;                   /* batches dispatched in current direction */
    /* Statistics */
    uint64_t            submitted;
    uint64_t            fetched;
    uint64_t            expired;
    uint64_t            front_merges;
    uint64_t            back_merges;
    uint64_t            total_merges;
};

/* ── CFQ scheduler data ──────────────────────────────────────────── */

struct cfq_queue {
    struct list_head      list;        /* link in priority-class list (rt/be/idle) */
    struct blk_request   *head;
    struct blk_request   *tail;
    int                   count;
    uint64_t              pid;         /* owning process PID */
    uint64_t              slice_start; /* tick when slice started */
    int                   dispatched;  /* requests dispatched in current slice */
    /* Enhanced CFQ fields */
    uint16_t              ioprio;      /* I/O priority from submitting process */
    uint32_t              slice_quota; /* time slice quota (ms, based on priority) */
    uint64_t              last_fetch;  /* last fetch time for think time estimation */
    uint8_t               is_sync;     /* 1 = queue serves synchronous I/O */
    uint8_t               seek_count;  /* number of seeks detected (sequential vs random) */
    uint64_t              last_lba;    /* last LBA for seek detection */
};

struct iosched_cfq_data {
    struct cfq_queue  queues[CFQ_QUEUES_MAX];
    int               queue_count;
    /* Per-priority-class active queue lists */
    struct list_head  rt_queues;      /* IOPRIO_CLASS_RT queues with pending requests */
    struct list_head  be_queues;      /* IOPRIO_CLASS_BE queues with pending requests */
    struct list_head  idle_queues;    /* IOPRIO_CLASS_IDLE queues with pending requests */
    struct cfq_queue *current_q;      /* queue currently being served */
    int               current_class;  /* current priority class being served */
    /* Idle timer for anticipatory scheduling */
    struct hrtimer    idle_timer;
    struct cfq_queue *idle_q;         /* queue being idled for (or NULL) */
    int               idle_expired;   /* set by timer callback when idle timeout fires */
    /* Write starvation tracking */
    uint64_t          read_dispatched;   /* read requests dispatched since last write */
    uint64_t          last_write_tick;   /* timer tick of last write dispatch */
    /* Statistics */
    uint64_t          idle_waits;     /* times we entered idle state */
    uint64_t          idle_hits;      /* process submitted more during idle window */
    uint64_t          queue_starved;  /* write starvation events triggered */
};

/* ── BFQ scheduler data ──────────────────────────────────────────── */

struct bfq_queue {
    struct list_head      list;        /* link in priority-class list (rt/be/idle) */
    struct blk_request   *head;
    struct blk_request   *tail;
    int                   count;
    uint64_t              pid;         /* owning process PID */
    uint16_t              ioprio;      /* I/O priority from submitting process */
    int                   budget;      /* remaining budget (sectors) */
    int                   initial_budget; /* starting budget for this queue */
    unsigned int          weight;      /* scheduling weight */
    uint8_t               is_sync;     /* 1 = serves synchronous I/O */
    uint64_t              last_fetch;  /* last fetch time (ns) for think-time */
    uint64_t              last_lba;    /* last LBA for seek detection */
};

struct iosched_bfq_data {
    struct bfq_queue  queues[BFQ_QUEUES_MAX];
    int               queue_count;
    /* Per-priority-class active queue lists */
    struct list_head  rt_queues;      /* IOPRIO_CLASS_RT */
    struct list_head  be_queues;      /* IOPRIO_CLASS_BE */
    struct list_head  idle_queues;    /* IOPRIO_CLASS_IDLE */
    struct bfq_queue *current_q;      /* queue currently being served */
    int               current_class;  /* current priority class */
    /* Low-latency mode: prioritize processes doing small I/O */
    int               low_latency;
    /* Idle timer for anticipatory scheduling */
    struct hrtimer    idle_timer;
    struct bfq_queue *idle_q;         /* queue being idled for (or NULL) */
    int               idle_expired;   /* set by timer callback on idle timeout */
    /* Write starvation tracking */
    uint64_t          read_dispatched; /* read requests since last write */
    uint64_t          last_write_tick; /* monotonic ns of last write dispatch */
    /* Statistics */
    uint64_t          budget_reassignments; /* total budget reassignments */
    uint64_t          idle_waits;
    uint64_t          idle_hits;
};

/* ── Per-device I/O scheduler queue ───────────────────────────────── */

struct iosched_kyber_domain {
    struct blk_request *head;
    struct blk_request *tail;
    int                 count;          /* pending requests in this domain */
    int                 tokens;         /* current available token budget */
    int                 tokens_min;     /* minimum token budget */
    int                 tokens_max;     /* maximum token budget */
    uint64_t            target_ns;      /* latency target in ns */
    uint64_t            last_token_tune;/* monotonic ns of last token adjustment */
    uint64_t            submitted;      /* total submissions */
    uint64_t            fetched;        /* total fetches from this domain */
    uint64_t            lat_avg;        /* average latency in ns (exponential smoothing) */
    uint64_t            lat_samples;    /* latency sample count for statistics */
};

struct iosched_kyber_data {
    struct iosched_kyber_domain domains[KYBER_MAX_DOMAINS];
    int                total_queued;    /* total pending across all domains */
    int                next_domain;     /* domain index for round-robin start */
};

struct iosched_queue {
    spinlock_t          lock;
    int                 dev_id;
    int                 policy;          /* IOSCHED_NOOP, _DEADLINE, _CFQ, _KYBER, _BFQ */
    const struct iosched_ops *ops;

    /* Deadline / NOOP: simple linked list of pending requests */
    struct blk_request *head;
    struct blk_request *tail;
    int                 queued_count;

    /* Policy-specific private data */
    union {
        struct iosched_noop_data     noop;
        struct iosched_deadline_data deadline;
        struct iosched_cfq_data     cfq;
        struct iosched_kyber_data   kyber;
        struct iosched_bfq_data     bfq;
    };
} __cacheline_aligned;

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
