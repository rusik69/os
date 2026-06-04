#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "types.h"
#include "waitqueue.h"

#define BLOCKDEV_MAX_DEVICES 32

#define BLOCKDEV_ATA    0
#define BLOCKDEV_AHCI   1
#define BLOCKDEV_USB0   16
#define BLOCKDEV_VIRTIO0 2
#define BLOCKDEV_RAMDISK 15

/* Block I/O request flags */
#define BLK_REQ_READ      (1ULL << 0)
#define BLK_REQ_WRITE     (1ULL << 1)
#define BLK_REQ_FLUSH     (1ULL << 2)
#define BLK_REQ_FUA       (1ULL << 3)  /* Force Unit Access */
#define BLK_REQ_PREFLUSH  (1ULL << 4)
#define BLK_REQ_DISCARD   (1ULL << 5)  /* Deallocate/TRIM — data not written */

/* Driver flags */
#define BLK_DRIVER_ASYNC  1   /* Driver handles completion asynchronously via blk_request_done() */

/* I/O scheduler types */
enum blk_scheduler {
    BLK_SCHED_NOOP = 0,      /* FIFO */
    BLK_SCHED_DEADLINE,      /* deadline with read-bias sorting */
    BLK_SCHED_COUNT
};

/* Block I/O statistics */
struct blockdev_stats {
    uint64_t read_ops;       /* number of read requests completed */
    uint64_t write_ops;      /* number of write requests completed */
    uint64_t read_sectors;   /* total sectors read */
    uint64_t write_sectors;  /* total sectors written */
    uint64_t read_ms;        /* total time spent reading (ms) */
    uint64_t write_ms;       /* total time spent writing (ms) */
};

/* Block I/O request */
struct blk_request {
    uint64_t      lba;
    uint32_t      count;      /* sector count */
    uint32_t      flags;      /* BLK_REQ_* flags */
    void         *buf;        /* data buffer (must be 512-byte aligned) */
    int           result;     /* filled by driver: 0 = success, <0 = error */
    uint8_t       dev_id;     /* target device */
    uint8_t       inflight;   /* 0 = queued, 1 = dispatched to driver */
    uint8_t       aborted;    /* 1 if cancelled */

    /* Completion support — driver calls blk_request_done() when finished */
    int           done;       /* set to 1 by blk_request_done() */
    struct wait_queue *done_wq;    /* non-NULL = synchronous caller waiting */

    /* Deadline scheduler fields */
    uint64_t      expiry;     /* deadline = submit_tick + timeout */
    uint16_t      ioprio;     /* I/O priority class+data of submitting process */
    struct blk_request *next; /* linked list in the request queue */
};

/* Per-device request queue */
struct blk_request_queue {
    spinlock_t      lock;
    struct blk_request *head;
    struct blk_request *tail;
    enum blk_scheduler  sched;
    uint8_t         dev_id;
    uint8_t         queued_count;   /* total requests in queue */
    uint8_t         inflight_count; /* requests dispatched to driver */
    uint8_t         batch_done;     /* deadline scheduler batch age */
};

/* Low-level driver interface (called by blockdev layer) */
typedef int (*blk_driver_submit_fn)(struct blk_request *req);
typedef int (*blk_driver_idle_fn)(struct blk_request_queue *q, int force_flush);

/* Legacy function pointer types (kept for backward compat) */
typedef int (*blockdev_read_fn)(uint32_t lba, uint8_t count, void *buf);
typedef int (*blockdev_write_fn)(uint32_t lba, uint8_t count, const void *buf);
typedef uint32_t (*blockdev_size_fn)(void);

/* Block device entry */
struct blockdev_entry {
    int     active;
    char    name[16];
    uint64_t sector_count;
    uint32_t sector_size;       /* usually 512 */

    /* Driver flags (BLK_DRIVER_ASYNC etc.) */
    int     flags;

    /* Low-level driver hooks */
    blk_driver_submit_fn submit_fn;   /* submit one request for processing */
    blk_driver_idle_fn   idle_fn;     /* optional: flush pending commands */

    /* I/O statistics */
    struct blockdev_stats stats;

    /* Transfer limits (Item 328: bio splitting for large requests) */
    uint32_t max_transfer;   /* maximum sectors per I/O (0 = unlimited) */
};

/* ── Public API ───────────────────────────────────────────────────── */

void           blockdev_init(void);
int            blockdev_register(int id, const char *name,
                                 blk_driver_submit_fn submit_fn,
                                 blk_driver_idle_fn idle_fn,
                                 uint64_t sector_count,
                                 int flags);

/* Legacy registration: wraps read/write/size into a submit_fn internally */
#if !defined(BLOCKDEV_NEW_ONLY)
int            blockdev_register_legacy(int id, const char *name,
                                        blockdev_read_fn read_fn,
                                        blockdev_write_fn write_fn,
                                        blockdev_size_fn size_fn);
#endif

int            blockdev_is_registered(int id);
int            blockdev_unregister(int id);
const char    *blockdev_name(int id);
uint64_t       blockdev_get_sectors(int id);
struct blk_request_queue *blockdev_get_queue(int id);

/* I/O scheduler selection */
int  blockdev_set_scheduler(int id, enum blk_scheduler sched);
enum blk_scheduler blockdev_get_scheduler(int id);

/* Request queue management — drivers call these */
struct blk_request *blk_request_alloc(void);
void  blk_request_free(struct blk_request *req);
void  blk_request_done(struct blk_request *req);   /* driver calls on completion */
int   blk_request_dequeue(struct blk_request_queue *q, struct blk_request **out);
struct blk_request *blk_request_deadline_peek(struct blk_request_queue *q);

/* Synchronous I/O (returns 0 on success, -errno on error) */
int  blk_submit_sync(int dev_id, uint64_t lba, uint32_t count,
                     void *buf, uint32_t flags);

/* Discard/TRIM — deallocate a range of LBAs (returns 0 on success, -errno on error) */
int  blockdev_discard(int dev_id, uint64_t lba, uint32_t count);

/* Async I/O (returns immediately; req->done is set on completion) */
int  blk_submit_async(struct blk_request *req);

/* Legacy wrapper API (kept for compatibility) */
static inline int blockdev_read_sectors(int id, uint32_t lba, uint8_t count, void *buf) {
    return blk_submit_sync(id, lba, count, buf, BLK_REQ_READ);
}
static inline int blockdev_write_sectors(int id, uint32_t lba, uint8_t count, const void *buf) {
    return blk_submit_sync(id, lba, count, (void*)buf, BLK_REQ_WRITE);
}

/* Block device statistics */
int blockdev_get_stats(int dev, struct blockdev_stats *s);

/* Internal: update statistics (called by drivers when a request completes) */
void blockdev_stats_update(int dev_id, int is_write, uint64_t sectors, uint64_t duration_ms);

/* Transfer limit configuration (Item 328: bio splitting for large requests) */
int  blockdev_set_max_transfer(int dev_id, uint32_t max_sectors);
uint32_t blockdev_get_max_transfer(int dev_id);

#endif /* BLOCKDEV_H */
