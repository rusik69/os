#ifndef FUSE_H
#define FUSE_H

#include "types.h"

/*
 * FUSE kernel interface definitions.
 *
 * This implements the kernel side of FUSE (Filesystem in Userspace).
 * A userspace daemon communicates with the kernel via /dev/fuse,
 * reading FUSE requests and writing FUSE responses.
 *
 * Based on the Linux FUSE protocol, simplified.
 */

/* Device path */
#define FUSE_DEVICE_PATH "/dev/fuse"

/* Minimum read buffer for FUSE device */
#define FUSE_MIN_READ_BUFFER 8192

/* ── FUSE operation codes ───────────────────────────────────────────── */
enum fuse_opcode {
    FUSE_LOOKUP     = 1,
    FUSE_FORGET     = 2,
    FUSE_GETATTR    = 3,
    FUSE_SETATTR    = 4,
    FUSE_READLINK   = 5,
    FUSE_SYMLINK    = 6,
    FUSE_MKNOD      = 8,
    FUSE_MKDIR      = 9,
    FUSE_UNLINK     = 10,
    FUSE_RMDIR      = 11,
    FUSE_RENAME     = 12,
    FUSE_LINK       = 13,
    FUSE_OPEN       = 14,
    FUSE_READ       = 15,
    FUSE_WRITE      = 16,
    FUSE_STATFS     = 17,
    FUSE_RELEASE    = 18,
    FUSE_FSYNC      = 20,
    FUSE_SETXATTR   = 21,
    FUSE_GETXATTR   = 22,
    FUSE_LISTXATTR  = 23,
    FUSE_REMOVEXATTR= 24,
    FUSE_FLUSH      = 25,
    FUSE_INIT       = 26,
    FUSE_OPENDIR    = 27,
    FUSE_READDIR    = 28,
    FUSE_RELEASEDIR = 29,
    FUSE_FSYNCDIR   = 30,
    FUSE_GETLK      = 31,
    FUSE_SETLK      = 32,
    FUSE_SETLKW     = 33,
    FUSE_ACCESS     = 34,
    FUSE_CREATE     = 35,
    FUSE_INTERRUPT  = 36,
    FUSE_BMAP       = 37,
    FUSE_DESTROY    = 38,
    FUSE_IOCTL      = 39,
    FUSE_POLL       = 40,
    FUSE_FALLOCATE  = 43,
};

/* FUSE major/minor version */
#define FUSE_KERNEL_VERSION     7
#define FUSE_KERNEL_MINOR_VERSION 23

/* FUSE request header */
struct fuse_in_header {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    uint32_t padding;
} __attribute__((packed));

/* FUSE response header */
struct fuse_out_header {
    uint32_t len;
    int32_t  error;
    uint64_t unique;
} __attribute__((packed));

/* FUSE_INIT request parameters */
struct fuse_init_in {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
} __attribute__((packed));

/* FUSE_INIT response parameters */
struct fuse_init_out {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
    uint16_t max_background;
    uint16_t congestion_threshold;
    uint32_t max_write;
    uint32_t time_gran;
    uint16_t max_pages;
    uint16_t map_alignment;
    uint32_t unused[8];
} __attribute__((packed));

/* FUSE attribute */
struct fuse_attr {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
    uint32_t blksize;
    uint32_t padding;
} __attribute__((packed));

/* FUSE entry out (LOOKUP, CREATE, MKNOD, etc.) */
struct fuse_entry_out {
    uint64_t nodeid;
    uint64_t generation;
    uint64_t entry_valid;
    uint64_t attr_valid;
    uint32_t entry_valid_nsec;
    uint32_t attr_valid_nsec;
    struct fuse_attr attr;
} __attribute__((packed));

/* FUSE_OPEN request */
struct fuse_open_in {
    uint32_t flags;
    uint32_t unused;
} __attribute__((packed));

/* FUSE_OPEN response */
struct fuse_open_out {
    uint64_t fh;
    uint32_t open_flags;
    uint32_t padding;
} __attribute__((packed));

/* FUSE_READ request */
struct fuse_read_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t read_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed));

/* FUSE_WRITE request */
struct fuse_write_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t write_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed));

/* FUSE_WRITE response */
struct fuse_write_out {
    uint32_t size;
    uint32_t padding;
} __attribute__((packed));

/* FUSE_RELEASE request */
struct fuse_release_in {
    uint64_t fh;
    uint32_t flags;
    uint32_t release_flags;
    uint64_t lock_owner;
} __attribute__((packed));

/* ── FUSE device state ──────────────────────────────────────────────── */

struct fuse_dev {
    int opened;            /* /dev/fuse has been opened by daemon */
    uint64_t unique;       /* next unique request ID */
    /* Request queue (simplified: single slot for now) */
    struct fuse_in_header *pending_req;
    void                  *pending_arg;  /* additional request data */
    int pending_arg_size;
};

/* ── FUSE dev (fuse_dev.c) API ───────────────────────────────────────── */

/**
 * fuse_dev_init — Register /dev/fuse character device.
 * Called once during fuse_init().
 */
void fuse_dev_init(void);

/**
 * fuse_dev_queue_request — Queue a FUSE request for the daemon.
 * @opcode:   FUSE operation code (FUSE_LOOKUP, FUSE_READ, etc.)
 * @nodeid:   FUSE node ID of the target inode
 * @arg:      Pointer to request-specific argument data (may be NULL)
 * @arg_size: Size of argument data in bytes
 * @out_unique: On success, receives the unique request ID (may be NULL)
 *
 * Allocates a request entry, fills the fuse_in_header, and queues it
 * for the daemon.  Wakes the daemon if it is blocked on /dev/fuse read.
 *
 * Returns 0 on success, or a negative errno on failure.
 * On success, the unique ID for this request is written to @out_unique
 * (if non-NULL), which should be passed to fuse_dev_wait_for_response.
 */
int fuse_dev_queue_request(uint32_t opcode, uint64_t nodeid,
                            const void *arg, int arg_size,
                            uint64_t *out_unique);

/**
 * fuse_dev_wait_for_response — Wait for the daemon's response.
 * @unique:          The unique ID from fuse_dev_queue_request (returned in hdr->unique)
 * @out_resp:        On success, receives allocated fuse_out_header (caller must kfree)
 * @out_resp_arg:    On success, receives optional response data (caller must kfree)
 * @out_resp_arg_size: On success, receives size of response arg data
 *
 * Blocks until the daemon writes a response matching @unique.
 * Returns 0 on success, or a negative errno on timeout/error.
 */
int fuse_dev_wait_for_response(uint64_t unique,
                                struct fuse_out_header **out_resp,
                                void **out_resp_arg,
                                int *out_resp_arg_size);

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialize FUSE subsystem */
void fuse_init(void);

/* Mount a FUSE filesystem — attaches to /dev/fuse.
 * Returns 0 on success, -1 on error. */
int fuse_mount(const char *mountpoint);

/* Unmount a FUSE filesystem */
int fuse_unmount(const char *mountpoint);

#endif /* FUSE_H */
