#ifndef VIRTIO_FS_H
#define VIRTIO_FS_H

#include "types.h"

/* ── Virtio FS device ID ────────────────────────────────────────────── */
#define VIRTIO_ID_FS         26

/* ── Virtio FS feature bits ─────────────────────────────────────────── */
#define VIRTIO_FS_F_FUSE_DAX  (1u << 0)  /* FUSE DAX window (memory-mapped) */
#define VIRTIO_FS_F_NOTIFY    (1u << 1)  /* FUSE notify support */
#define VIRTIO_FS_F_DAX       (1u << 2)  /* DAX window support */

/* ── FUSE opcodes used by virtio-fs ─────────────────────────────────── */
#define FUSE_LOOKUP           1
#define FUSE_GETATTR          3
#define FUSE_READ             9
#define FUSE_WRITE            10
#define FUSE_READDIR          28
#define FUSE_OPEN             14
#define FUSE_RELEASE          18
#define FUSE_OPENDIR          27
#define FUSE_RELEASEDIR       29
#define FUSE_STATFS           17
#define FUSE_CREATE           35
#define FUSE_MKDIR            13
#define FUSE_UNLINK           11
#define FUSE_RMDIR            12
#define FUSE_RENAME           38
#define FUSE_INIT             4096
#define FUSE_DESTROY          4097

/* ── FUSE request/response header (wire format) ─────────────────────── */
#pragma pack(push, 1)
struct fuse_in_header {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    uint32_t padding;
};

struct fuse_out_header {
    uint32_t len;
    int32_t  error;
    uint64_t unique;
};

/* FUSE attribute structure */
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
};

/* FUSE_LOOKUP response: entry_out + nodeid */
struct fuse_entry_out {
    uint64_t nodeid;
    uint64_t generation;
    uint64_t entry_valid;
    uint64_t attr_valid;
    uint32_t entry_valid_nsec;
    uint32_t attr_valid_nsec;
    struct fuse_attr attr;
};

/* FUSE_GETATTR request: getattr_in */
struct fuse_getattr_in {
    uint32_t getattr_flags;
    uint32_t dummy;
    uint64_t fh;
};

/* FUSE_GETATTR response: attr_out */
struct fuse_attr_out {
    uint64_t attr_valid;
    uint32_t attr_valid_nsec;
    uint32_t dummy;
    struct fuse_attr attr;
};

/* FUSE_OPEN request: open_in */
struct fuse_open_in {
    uint32_t flags;
    uint32_t unused;
};

/* FUSE_OPEN response: open_out */
struct fuse_open_out {
    uint64_t fh;
    uint32_t open_flags;
    uint32_t padding;
};

/* FUSE_READ request: read_in */
struct fuse_read_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t read_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

/* FUSE_WRITE request: write_in */
struct fuse_write_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t write_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

/* FUSE_WRITE response: write_out */
struct fuse_write_out {
    uint32_t size;
    uint32_t padding;
};

/* FUSE_READDIR request: readdir_in (no extra payload beyond fuse_in_header) */
struct fuse_readdir_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t padding;
};

/* FUSE_INIT request: init_in */
struct fuse_init_in {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
};

/* FUSE_INIT response: init_out */
struct fuse_init_out {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
    uint16_t max_background;
    uint16_t congestion_threshold;
    uint32_t max_write;
    uint32_t time_gran;
    uint32_t unused[9];
};
#pragma pack(pop)

/* ── Virtio FS configuration space ──────────────────────────────────── */
#define VIRTIO_FS_CONFIG_TAG_LEN   36
#define VIRTIO_FS_CONFIG_NUM_REQ_QUEUES  1
#define VIRTIO_FS_CONFIG_NUM_EVT_QUEUES  0

/* ── Virtio FS device state ─────────────────────────────────────────── */
struct virtio_fs_device {
    uint16_t   iobase;           /* PCI I/O base */
    int        present;
    char       tag[VIRTIO_FS_CONFIG_TAG_LEN]; /* filesystem tag */
    int        hiprio_queues;    /* high-priority request queues */
    int        req_queues;       /* request queues */
    int        evt_queues;       /* event queues */

    /* Host-side: the directory being exported */
    char       host_dir[256];
    char       mount_point[256];
};

/* ── Public API ─────────────────────────────────────────────────────── */
int virtio_fs_init(void);
int virtio_fs_mount(const char *host_dir, const char *mount_point);
int virtio_fs_handle_request(int vq_idx);
void virtio_fs_cleanup(void);

/* ── Additional FUSE structs (defined here if fuse.h not included) ── */
#ifndef HAVE_FUSE_FSYNC_IN
#define HAVE_FUSE_FSYNC_IN
struct fuse_fsync_in {
    uint64_t fh;
    uint32_t fsync_flags;
    uint32_t padding;
} __attribute__((packed));

struct fuse_release_in {
    uint64_t fh;
    uint32_t flags;
    uint32_t release_flags;
    uint64_t lock_owner;
} __attribute__((packed));

struct fuse_statfs_out {
    struct {
        uint64_t blocks;
        uint64_t bfree;
        uint64_t bavail;
        uint64_t files;
        uint64_t ffree;
        uint32_t bsize;
        uint32_t frsize;
        uint32_t namelen;
        uint32_t frsize2;
    } st;
} __attribute__((packed));
#endif /* HAVE_FUSE_FSYNC_IN */

/* Opcodes not in header */
#ifndef FUSE_FLUSH
#define FUSE_FLUSH        25
#endif
#ifndef FUSE_FSYNC
#define FUSE_FSYNC        20
#endif

#endif /* VIRTIO_FS_H */
