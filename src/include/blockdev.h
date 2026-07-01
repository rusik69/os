#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "types.h"
#include "waitqueue.h"

#define BLOCKDEV_MAX_DEVICES 32

#define BLOCKDEV_ATA    0
#define BLOCKDEV_AHCI   1
#define BLOCKDEV_USB0   16
#define BLOCKDEV_USB1   17  /* USB Attached SCSI (UAS) device */
#define BLOCKDEV_VIRTIO0 2
#define BLOCKDEV_RAMDISK 15
#define BLOCKDEV_LOOP0   3
#define BLOCKDEV_LOOP1   4
#define BLOCKDEV_LOOP2   5
#define BLOCKDEV_LOOP3   6
#define BLOCKDEV_LOOP_MAX 4
#define BLOCKDEV_PMEM0   7  /* First PMEM (NVDIMM) device */

/* Block I/O request flags */
#define BLK_REQ_READ      (1ULL << 0)
#define BLK_REQ_WRITE     (1ULL << 1)
#define BLK_REQ_FLUSH     (1ULL << 2)
#define BLK_REQ_FUA       (1ULL << 3)  /* Force Unit Access */
#define BLK_REQ_PREFLUSH  (1ULL << 4)
#define BLK_REQ_DISCARD   (1ULL << 5)  /* Deallocate/TRIM — data not written */
#define BLK_REQ_SYNC      (1ULL << 6)  /* Synchronous I/O — process waits for completion */

/* Driver flags */
#define BLK_DRIVER_ASYNC  1   /* Driver handles completion asynchronously via blk_request_done() */

/* I/O scheduler types */
enum blk_scheduler {
    BLK_SCHED_NOOP = 0,      /* FIFO */
    BLK_SCHED_DEADLINE,      /* deadline with read-bias sorting */
    BLK_SCHED_CFQ,           /* Complete Fair Queueing */
    BLK_SCHED_COUNT
};

/* Block I/O statistics — Linux /sys/block/<dev>/stat compatible */
struct blockdev_stats {
    /* Basic I/O */
    uint64_t read_ops;       /* number of read requests completed */
    uint64_t write_ops;      /* number of write requests completed */
    uint64_t read_sectors;   /* total sectors read */
    uint64_t write_sectors;  /* total sectors written */
    uint64_t read_ms;        /* total time spent reading (ms) */
    uint64_t write_ms;       /* total time spent writing (ms) */

    /* Merge statistics */
    uint64_t read_merges;    /* adjacent read requests merged */
    uint64_t write_merges;   /* adjacent write requests merged */

    /* In-flight / queue depth tracking */
    uint32_t io_in_flight;   /* current number of I/Os in flight */
    uint64_t io_ticks;       /* cumulative jiffies with at least one I/O in flight */
    uint64_t weighted_io_ticks; /* cumulative (io_in_flight * elapsed) ms */

    /* Discard (TRIM) statistics */
    uint64_t discard_ops;      /* number of discard requests completed */
    uint64_t discard_sectors;  /* total sectors discarded */
    uint64_t discard_ms;       /* total time spent discarding (ms) */

    /* Flush / FUA statistics */
    uint64_t flush_ops;        /* number of flush requests completed */
    uint64_t flush_ms;         /* total time spent flushing (ms) */
} __cacheline_aligned;

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
} __cacheline_aligned;

/* Low-level driver interface (called by blockdev layer) */
typedef int (*blk_driver_submit_fn)(struct blk_request *req);
typedef int (*blk_driver_idle_fn)(struct blk_request_queue *q, int force_flush);

/* Legacy function pointer types (kept for backward compat) */
typedef int (*blockdev_read_fn)(uint32_t lba, uint8_t count, void *buf);
typedef int (*blockdev_write_fn)(uint32_t lba, uint8_t count, const void *buf);
typedef uint32_t (*blockdev_size_fn)(void);

/* SCSI command callback type for pass-through drivers.
 * Forward declaration — used before the typedef in struct blockdev_entry. */
typedef int (*scsi_submit_cmd_fn)(int dev_id,
                                   const uint8_t *cdb, int cdb_len,
                                   void *data, int data_len,
                                   int dir,
                                   uint8_t *sense, int *sense_len,
                                   int timeout_ms);

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

    /* SCSI pass-through command callback (NULL if not SCSI-capable) */
    scsi_submit_cmd_fn scsi_cmd_fn;
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

/* Reset block device statistics to zero */
void blockdev_stats_reset(int dev_id);

/* Internal: update statistics (called by drivers when a request completes)
 * @dev_id:     device ID
 * @is_write:   nonzero for write, zero for read (ignored for discard/flush)
 * @is_discard: nonzero for discard/TRIM operations
 * @is_flush:   nonzero for flush/preflush/FUA operations
 * @sectors:    number of sectors transferred/discarded
 * @duration_ms: time taken in milliseconds
 */
void blockdev_stats_update(int dev_id, int is_write, int is_discard,
                           int is_flush, uint64_t sectors, uint64_t duration_ms);

/* Format block device statistics into a string (for /proc/diskstats style output).
 * Returns the number of characters written (excluding NUL).
 * @buf:   output buffer
 * @size:  buffer size
 * @dev:   device ID
 */
int blockdev_stats_format(char *buf, int size, int dev);

/* Find a block device by name (e.g., "sda", "nvme0n1").
 * Strips "/dev/" prefix if present.
 * Returns device ID on success, -1 on not found. */
int blockdev_find_by_name(const char *name);

/* Transfer limit configuration (Item 328: bio splitting for large requests) */
int  blockdev_set_max_transfer(int dev_id, uint32_t max_sectors);
uint32_t blockdev_get_max_transfer(int dev_id);

/* ── SCSI generic passthrough (SG_IO) ───────────────────────────── */

/* Maximum CDB (SCSI command descriptor block) size */
#define SG_MAX_CDB_SIZE   32
#define SG_MAX_SENSE_SIZE 96

/* SG_IO ioctl command code (Linux-compatible) */
#define SG_IO  0x2285

/* Data transfer direction for SG_IO */
#define SG_DXFER_NONE      (-1)
#define SG_DXFER_TO_DEV    (-2)   /* data FROM CPU TO device (write) */
#define SG_DXFER_FROM_DEV  (-3)   /* data FROM device TO CPU (read) */
#define SG_DXFER_TO_FROM_DEV (-4) /* bidirectional */

/* sg_io_hdr — SCSI generic I/O header (Linux-compatible subset).
 * Userspace submits a SCSI CDB and receives sense/status through this struct. */
struct sg_io_hdr {
    int             interface_id;    /* must be 'S' (0x53) */
    int             dxfer_direction; /* SG_DXFER_* */
    unsigned char   cmd_len;         /* length of CDB */
    unsigned char   mx_sb_len;       /* max sense buffer length */
    unsigned short  iovec_count;     /* must be 0 for simple implementation */
    unsigned int    dxfer_len;       /* data transfer length */
    void           *dxferp;          /* pointer to data buffer (user) */
    unsigned char   *cmdp;           /* pointer to CDB (user) */
    unsigned char   *sbp;            /* pointer to sense buffer (user) */
    unsigned int    timeout;         /* timeout in ms */
    unsigned int    flags;
    unsigned char   pack_id;
    void           *usr_ptr;
    unsigned char   status;          /* SCSI status filled by kernel */
    unsigned char   masked_status;
    unsigned char   msg_status;
    unsigned char   sb_len_wr;       /* sense data length written */
    unsigned short  host_status;
    unsigned short  driver_status;
    int             resid;           /* residual count */
    unsigned int    duration;
    unsigned int    info;
};

/* SCSI command callback for pass-through drivers.
 * Returns 0 on success, negative errno on failure.
 * (typedef is at the top of this file, before struct blockdev_entry) */

/* Register a SCSI command callback for a block device.
 * @dev_id: block device ID
 * @fn: callback function (NULL to unregister)
 * Returns 0 on success, -1 if device not found. */
int blockdev_register_scsi_cmd(int dev_id, scsi_submit_cmd_fn fn);

/* Submit a SCSI command via passthrough.
 * Used internally by the SG_IO ioctl handler.
 * Returns 0 on success, negative errno on failure. */
int blockdev_scsi_submit(int dev_id,
                          const uint8_t *cdb, int cdb_len,
                          void *data, int data_len,
                          int dir,
                          uint8_t *sense, int *sense_len,
                          int timeout_ms);

#endif /* BLOCKDEV_H */
