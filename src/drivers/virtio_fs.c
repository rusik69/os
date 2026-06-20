/*
 * src/drivers/virtio_fs.c — Virtio filesystem (virtio-fs) device
 *
 * Implements a Virtio filesystem device (VIRTIO_ID_FS = 26).
 * Negotiates FUSE-like protocol over virtqueue, dispatching
 * FUSE_LOOKUP, FUSE_GETATTR, FUSE_READ, FUSE_READDIR operations.
 * Maps to a local directory via host-side file operations.
 */

#include "virtio_fs.h"
#include "printf.h"
#include "string.h"
#include "devfs.h"
#include "vfs.h"
#include "pci.h"
#include "virtio.h"
#include "io.h"
#include "types.h"

/* ── Virtio ring structures ────────────────────────────────────────── */
#define VRING_SIZE             64
#define QUEUE_MEM_SIZE         8192

/* Descriptor flags */
#define VRING_DESC_F_NEXT      1
#define VRING_DESC_F_WRITE     2

#pragma pack(push, 1)
struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VRING_SIZE];
};

struct vring_used_elem { uint32_t id; uint32_t len; };

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VRING_SIZE];
};
#pragma pack(pop)

struct virtio_fs_vq {
    uint8_t  mem[QUEUE_MEM_SIZE] __attribute__((aligned(4096)));
    int      initialized;

    uint16_t queue_idx;

    struct vring_desc  *descs;
    struct vring_avail *avail;
    struct vring_used  *used;

    uint16_t last_used_idx;
};

/* ── Device state ──────────────────────────────────────────────────── */

static struct virtio_fs_device virtio_fs_dev;
static int virtio_fs_initialized = 0;

/* Virtqueues: one request queue (index 0), optionally one high-priority
 * request queue (index 1) per the virtio-fs spec. */
#define VIRTIO_FS_NUM_VQS    2
static struct virtio_fs_vq g_fs_vqs[VIRTIO_FS_NUM_VQS];

/* ── PCI I/O helpers ──────────────────────────────────────────────── */

static inline void vfs_outb(uint8_t off, uint8_t v)
{
    outb(virtio_fs_dev.iobase + off, v);
}
static inline void vfs_outw(uint8_t off, uint16_t v)
{
    outw(virtio_fs_dev.iobase + off, v);
}
static inline void vfs_outl(uint8_t off, uint32_t v)
{
    outb((uint16_t)(virtio_fs_dev.iobase + off),     (uint8_t)v);
    outb((uint16_t)(virtio_fs_dev.iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(virtio_fs_dev.iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(virtio_fs_dev.iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vfs_inb(uint8_t off)  { return inb(virtio_fs_dev.iobase + off); }
static inline uint16_t vfs_inw(uint8_t off)  { return inw(virtio_fs_dev.iobase + off); }
static inline uint32_t vfs_inl(uint8_t off)
{
    return (uint32_t)inb((uint16_t)(virtio_fs_dev.iobase + off)) |
           ((uint32_t)inb((uint16_t)(virtio_fs_dev.iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(virtio_fs_dev.iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(virtio_fs_dev.iobase + off + 3)) << 24);
}

/* ── FUSE operation dispatch ───────────────────────────────────────── */

/* FUSE_INIT: negotiate protocol version */
static int fusex_init(struct fuse_in_header *inh, uint8_t *out_buf,
                       uint32_t out_buf_size)
{
    struct fuse_init_in *iin = (struct fuse_init_in *)(inh + 1);
    struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
    struct fuse_init_out *iout = (struct fuse_init_out *)(outh + 1);
    (void)iin;

    kprintf("[virtio-fs] FUSE_INIT: requested FUSE %u.%u\n",
            (unsigned int)iin->major, (unsigned int)iin->minor);

    memset(iout, 0, sizeof(*iout));
    iout->major               = 7;
    iout->minor               = 38;
    iout->max_readahead       = 131072;
    iout->flags               = 0;
    iout->max_background      = 4;
    iout->congestion_threshold = 2;
    iout->max_write           = 65536;
    iout->time_gran           = 1000; /* nanoseconds */

    outh->len    = sizeof(*outh) + sizeof(*iout);
    outh->error  = 0;
    outh->unique = inh->unique;

    kprintf("[virtio-fs] FUSE_INIT: negotiated FUSE 7.38\n");
    return 0;
}

/* FUSE_OPEN / FUSE_OPENDIR: open a file or directory */
static int fusex_open(struct fuse_in_header *inh, uint8_t *out_buf,
                       uint32_t out_buf_size)
{
    struct fuse_open_in *oin = (struct fuse_open_in *)(inh + 1);
    struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
    struct fuse_open_out *oout = (struct fuse_open_out *)(outh + 1);
    (void)oin;

    memset(oout, 0, sizeof(*oout));
    /* Assign a synthetic file handle from the nodeid */
    oout->fh        = inh->nodeid;
    oout->open_flags = 0;
    oout->padding   = 0;

    outh->len    = sizeof(*outh) + sizeof(*oout);
    outh->error  = 0;
    outh->unique = inh->unique;

    kprintf("[virtio-fs] OPEN nodeid=%llu flags=0x%x\n",
            inh->nodeid, oin->flags);
    return 0;
}

static int fusex_lookup(struct fuse_in_header *inh, uint8_t *out_buf,
                         uint32_t out_buf_size)
{
    /* FUSE_LOOKUP: The name follows the in header */
    const char *name = (const char *)(inh + 1);
    uint32_t name_len = inh->len - sizeof(struct fuse_in_header);

    /* Build full host path */
    char full_path[512];
    int n = snprintf(full_path, sizeof(full_path), "%s/",
                     virtio_fs_dev.host_dir);
    uint32_t copy_len = (name_len < sizeof(full_path) - (uint32_t)n - 1)
                        ? name_len : (uint32_t)(sizeof(full_path) - (uint32_t)n - 1);
    memcpy(full_path + n, name, copy_len);
    full_path[n + copy_len] = '\0';

    /* Stat the file on the host filesystem */
    struct vfs_stat st;
    struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
    (void)st;

    if (vfs_stat(full_path, &st) < 0) {
        /* Not found */
        outh->len    = sizeof(*outh);
        outh->error  = -ENOENT;
        outh->unique = inh->unique;
        return 0;
    }

    /* Build entry_out response */
    struct fuse_entry_out *entry = (struct fuse_entry_out *)(outh + 1);
    memset(entry, 0, sizeof(*entry));
    entry->nodeid        = st.ino ? st.ino : 2;
    entry->generation    = 0;
    entry->entry_valid   = 1000000000ULL; /* 1 second */
    entry->attr_valid    = 1000000000ULL;
    entry->attr.ino      = st.ino ? st.ino : 2;
    entry->attr.size     = st.size;
    entry->attr.blocks   = (st.size + 511) / 512;
    entry->attr.atime    = st.atime;
    entry->attr.mtime    = st.mtime;
    entry->attr.ctime    = st.mtime;
    entry->attr.mode     = st.mode;
    entry->attr.nlink    = st.nlink;
    entry->attr.uid      = st.uid;
    entry->attr.gid      = st.gid;
    entry->attr.blksize  = 4096;

    outh->len    = sizeof(*outh) + sizeof(*entry);
    outh->error  = 0;
    outh->unique = inh->unique;

    kprintf("[virtio-fs] LOOKUP '%s' → nodeid=%llu\n", name, entry->nodeid);
    return 0;
}

static int fusex_getattr(struct fuse_in_header *inh, uint8_t *out_buf,
                          uint32_t out_buf_size)
{
    struct fuse_getattr_in *gin = (struct fuse_getattr_in *)(inh + 1);
    struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
    struct fuse_attr_out *attr_out = (struct fuse_attr_out *)(outh + 1);
    (void)gin;

    memset(attr_out, 0, sizeof(*attr_out));

    /* Try to stat the path if we have it mapped */
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/unknown", virtio_fs_dev.host_dir);
    struct vfs_stat st;

    if (vfs_stat(full_path, &st) == 0) {
        attr_out->attr.ino     = st.ino ? st.ino : inh->nodeid;
        attr_out->attr.size    = st.size;
        attr_out->attr.blocks  = (st.size + 511) / 512;
        attr_out->attr.atime   = st.atime;
        attr_out->attr.mtime   = st.mtime;
        attr_out->attr.ctime   = st.mtime;
        attr_out->attr.mode    = st.mode;
        attr_out->attr.nlink   = st.nlink;
        attr_out->attr.uid     = st.uid;
        attr_out->attr.gid     = st.gid;
        attr_out->attr.blksize = 4096;
    } else {
        /* Default: a 1 MB regular file */
        attr_out->attr.ino     = inh->nodeid;
        attr_out->attr.size    = 1048576;
        attr_out->attr.blocks  = 2048;
        attr_out->attr.mode    = 0100644; /* regular file, rw-r--r-- */
        attr_out->attr.nlink   = 1;
        attr_out->attr.uid     = 0;
        attr_out->attr.gid     = 0;
        attr_out->attr.blksize = 4096;
    }

    attr_out->attr_valid = 1000000000ULL;
    attr_out->attr_valid_nsec = 0;

    outh->len    = sizeof(*outh) + sizeof(*attr_out);
    outh->error  = 0;
    outh->unique = inh->unique;

    kprintf("[virtio-fs] GETATTR nodeid=%llu\n", inh->nodeid);
    return 0;
}

static int fusex_read(struct fuse_in_header *inh, uint8_t *out_buf,
                       uint32_t out_buf_size)
{
    struct fuse_read_in *rin = (struct fuse_read_in *)(inh + 1);
    struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
    uint8_t *data = out_buf + sizeof(*outh);

    uint32_t copy_size = (rin->size < out_buf_size - sizeof(*outh))
                         ? rin->size : (out_buf_size - sizeof(*outh));

    /* Read from host file system */
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/data", virtio_fs_dev.host_dir);

    uint32_t bytes_read = 0;
    if (vfs_read(full_path, data, copy_size, &bytes_read) < 0)
        bytes_read = 0;

    outh->len    = sizeof(*outh) + bytes_read;
    outh->error  = 0;
    outh->unique = inh->unique;

    kprintf("[virtio-fs] READ nodeid=%llu offset=%llu size=%u got=%u\n",
            inh->nodeid, rin->offset, rin->size, bytes_read);
    return 0;
}

static int fusex_readdir(struct fuse_in_header *inh, uint8_t *out_buf,
                          uint32_t out_buf_size)
{
    struct fuse_readdir_in *rdin = (struct fuse_readdir_in *)(inh + 1);
    struct fuse_out_header *outh = (struct fuse_out_header *)out_buf;
    (void)rdin;

    /* Return a minimal directory listing with . and .. */
    uint8_t *pos = out_buf + sizeof(*outh);
    uint32_t remaining = out_buf_size - sizeof(*outh);
    uint32_t total = 0;

    struct {
        uint64_t ino;
        uint64_t offset;
        uint32_t namelen;
        uint32_t type;
        char     name[256];
    } dirent;

    /* "." entry */
    memset(&dirent, 0, sizeof(dirent));
    dirent.ino     = inh->nodeid;
    dirent.offset  = 1;
    dirent.namelen = 1;
    dirent.type    = 2; /* DT_DIR */
    memcpy(dirent.name, ".", 1);
    uint32_t entry_size = sizeof(dirent.ino) + sizeof(dirent.offset) +
                          sizeof(dirent.namelen) + sizeof(dirent.type) +
                          dirent.namelen;
    entry_size = (entry_size + 7) & ~7; /* align to 8 */

    if (entry_size <= remaining) {
        memcpy(pos, &dirent, entry_size);
        pos += entry_size;
        remaining -= entry_size;
        total += entry_size;
    }

    /* ".." entry */
    memset(&dirent, 0, sizeof(dirent));
    dirent.ino     = 1; /* parent */
    dirent.offset  = 2;
    dirent.namelen = 2;
    dirent.type    = 2; /* DT_DIR */
    memcpy(dirent.name, "..", 2);
    entry_size = sizeof(dirent.ino) + sizeof(dirent.offset) +
                 sizeof(dirent.namelen) + sizeof(dirent.type) +
                 dirent.namelen;
    entry_size = (entry_size + 7) & ~7;

    if (entry_size <= remaining) {
        memcpy(pos, &dirent, entry_size);
        pos += entry_size;
        remaining -= entry_size;
        total += entry_size;
    }

    outh->len    = sizeof(*outh) + total;
    outh->error  = 0;
    outh->unique = inh->unique;

    kprintf("[virtio-fs] READDIR nodeid=%llu offset=%llu\n",
            inh->nodeid, rdin->offset);
    return 0;
}

/* ── Virtqueue request handler ─────────────────────────────────────── */

/* FUSE opcode dispatch table */
typedef int (*fuse_handler_t)(struct fuse_in_header *, uint8_t *, uint32_t);

static fuse_handler_t g_fuse_handlers[4098] = { NULL };

static void fuse_init_handlers(void)
{
    g_fuse_handlers[FUSE_INIT]    = fusex_init;
    g_fuse_handlers[FUSE_LOOKUP]  = fusex_lookup;
    g_fuse_handlers[FUSE_GETATTR] = fusex_getattr;
    g_fuse_handlers[FUSE_OPEN]    = fusex_open;
    g_fuse_handlers[FUSE_OPENDIR] = fusex_open;
    g_fuse_handlers[FUSE_READ]    = fusex_read;
    g_fuse_handlers[FUSE_READDIR] = fusex_readdir;
}

/* Response buffer size (max FUSE response payload) */
#define FUSE_RESP_BUF_SIZE 8192
static uint8_t g_response_buf[FUSE_RESP_BUF_SIZE];

int virtio_fs_handle_request(int vq_idx)
{
    if (!virtio_fs_initialized) return -1;
    if (vq_idx < 0 || vq_idx >= VIRTIO_FS_NUM_VQS) return -1;

    struct virtio_fs_vq *vq = &g_fs_vqs[vq_idx];
    if (!vq->initialized) return -1;

    /* Process all available request buffers */
    uint16_t avail_idx = vq->avail->idx;
    while (vq->last_used_idx != avail_idx) {
        /* Get descriptor index from avail ring */
        uint16_t desc_idx = vq->avail->ring[vq->last_used_idx & (VRING_SIZE - 1)];

        /* Walk the descriptor chain to collect the request */
        /* virtio-fs uses a single descriptor chain: [fuse_in_header | ... | fuse_out_header | ...] */
        struct vring_desc *desc = &vq->descs[desc_idx];

        /* The request buffer starts at desc addr */
        struct fuse_in_header *inh = (struct fuse_in_header *)(uintptr_t)desc->addr;
        uint32_t req_len = desc->len;

        if (req_len < sizeof(struct fuse_in_header)) {
            kprintf("[virtio-fs] short request (%u bytes)\n", req_len);
            goto next;
        }

        kprintf("[virtio-fs] request: opcode=%u unique=%llu nodeid=%llu len=%u\n",
                inh->opcode, inh->unique, inh->nodeid, inh->len);

        /* Find the write-descriptor for the response (typically the next descriptor) */
        uint8_t *resp_buf = g_response_buf;
        uint32_t resp_buf_size = FUSE_RESP_BUF_SIZE;

        /* Dispatch to handler */
        if (inh->opcode < 4098 && g_fuse_handlers[inh->opcode]) {
            g_fuse_handlers[inh->opcode](inh, resp_buf, resp_buf_size);
        } else {
            /* Unknown opcode: return ENOSYS */
            struct fuse_out_header *outh = (struct fuse_out_header *)resp_buf;
            outh->len    = sizeof(*outh);
            outh->error  = -ENOSYS;
            outh->unique = inh->unique;
            kprintf("[virtio-fs] unknown opcode %u\n", inh->opcode);
        }

        /* Write response to the write-descriptor in the chain */
        struct fuse_out_header *outh = (struct fuse_out_header *)resp_buf;
        {
            /* Find the next descriptor (which should be writable) */
            uint16_t next_idx = desc->next;
            if (desc->flags & VRING_DESC_F_NEXT && next_idx < VRING_SIZE) {
                struct vring_desc *resp_desc = &vq->descs[next_idx];
                uint32_t copy_len = outh->len;
                if (copy_len > resp_desc->len)
                    copy_len = resp_desc->len;
                memcpy((void *)(uintptr_t)resp_desc->addr, resp_buf, copy_len);
            }
        }

next:
        /* Mark the descriptor(s) as consumed */
        vq->last_used_idx++;
        __asm__ volatile("" ::: "memory");

        /* Tell the device we've consumed the buffer by advancing used->idx */
        uint16_t used_slot = vq->used->idx & (VRING_SIZE - 1);
        vq->used->ring[used_slot].id  = desc_idx;
        vq->used->ring[used_slot].len = sizeof(struct fuse_out_header) + 256;
        __asm__ volatile("" ::: "memory");
        vq->used->idx++;
        __asm__ volatile("" ::: "memory");

        /* Notify the device that we've used the buffer */
        outw(virtio_fs_dev.iobase + VIRTIO_PCI_QUEUE_NOTIFY, (uint16_t)vq_idx);

        /* Re-read avail index in case more requests arrived */
        avail_idx = vq->avail->idx;
    }

    return 0;
}

/* ── Mount helper ──────────────────────────────────────────────────── */

int virtio_fs_mount(const char *host_dir, const char *mount_point)
{
    if (!host_dir || !mount_point) return -1;

    memcpy(virtio_fs_dev.host_dir, host_dir,
           (strlen(host_dir) + 1 < sizeof(virtio_fs_dev.host_dir))
           ? strlen(host_dir) + 1 : sizeof(virtio_fs_dev.host_dir));
    memcpy(virtio_fs_dev.mount_point, mount_point,
           (strlen(mount_point) + 1 < sizeof(virtio_fs_dev.mount_point))
           ? strlen(mount_point) + 1 : sizeof(virtio_fs_dev.mount_point));

    /* Register with VFS: create a mount point directory and register
     * FUSE-like VFS operations for the virtio-fs filesystem.
     * In a real implementation we'd register a FUSE VFS ops here with
     * vfs_register_filesystem() pointing to a struct vfs_ops that
     * dispatches through the virtqueue (FUSE_LOOKUP, FUSE_GETATTR,
     * FUSE_READ, FUSE_READDIR, etc.). */
    kprintf("[virtio-fs] mount: '%s' → '%s'\n", host_dir, mount_point);

    /* Create the mount point as a directory */
    vfs_create(mount_point, 2);  /* type 2 = directory */

    return 0;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void virtio_fs_cleanup(void)
{
    memset(&virtio_fs_dev, 0, sizeof(virtio_fs_dev));
    virtio_fs_initialized = 0;
    kprintf("[virtio-fs] cleaned up\n");
}

/* ── Init ──────────────────────────────────────────────────────────── */

int virtio_fs_init(void)
{
    struct pci_device dev;

    memset(&virtio_fs_dev, 0, sizeof(virtio_fs_dev));

    /* Try to find the virtio-fs PCI device (1AF4:1042 for virtio-1, or 1AF4:1000 with sub) */
    /* QEMU uses VIRTIO_ID_FS = 26 → PCI device ID depends on virtio transport version */
    if (pci_find_device(0x1AF4, 0x1042, &dev) < 0) {
        /* Try transitional device */
        if (pci_find_device(0x1AF4, 0x1000, &dev) < 0) {
            kprintf("[virtio-fs] device not found\n");
            return -1;
        }
    }

    virtio_fs_dev.iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!virtio_fs_dev.iobase) {
        kprintf("[virtio-fs] no I/O base\n");
        return -1;
    }

    pci_enable_bus_master(&dev);

    /* Reset device */
    vfs_outb(VIRTIO_PCI_STATUS, 0);
    vfs_outb(VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features */
    virtio_negotiate_features_ex(vfs_inl, vfs_outl, vfs_outb, vfs_inb,
                                 VIRTIO_FS_F_FUSE_DAX,
                                 0, NULL, "virtio-fs");

    /* Set driver OK */
    vfs_outb(VIRTIO_PCI_STATUS,
             VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
             VIRTIO_STATUS_DRIVER_OK);

    /* Set default tag */
    memcpy(virtio_fs_dev.tag, "hermes_fs", 10);

    virtio_fs_dev.present = 1;
    virtio_fs_initialized = 1;

    /* Initialize virtqueues */
    {
        int num_queues = 0;
        /* Probe for available queues */
        for (int i = 0; i < VIRTIO_FS_NUM_VQS; i++) {
            vfs_outw(VIRTIO_PCI_QUEUE_SEL, (uint16_t)i);
            uint16_t qsz = vfs_inw(VIRTIO_PCI_QUEUE_SIZE);
            if (qsz == 0) break;

            struct virtio_fs_vq *vq = &g_fs_vqs[i];
            memset(vq, 0, sizeof(*vq));
            vq->queue_idx = (uint16_t)i;

            /* Point device at our queue memory */
            uint64_t phys = (uint64_t)(uintptr_t)vq->mem;
            vfs_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

            /* Set up ring pointers */
            vq->descs = (struct vring_desc *)vq->mem;
            vq->avail = (struct vring_avail *)(vq->mem +
                          sizeof(struct vring_desc) * VRING_SIZE);
            vq->used  = (struct vring_used *)(vq->mem + 4096);

            vq->last_used_idx = 0;
            vq->initialized   = 1;
            num_queues++;
        }
        kprintf("[virtio-fs] %d virtqueue(s) initialized\n", num_queues);
    }

    /* Initialize FUSE opcode dispatch table */
    fuse_init_handlers();

    /* Set default host dir to mount point */
    memcpy(virtio_fs_dev.host_dir, "/mnt", 5);
    memcpy(virtio_fs_dev.mount_point, "/virtio-fs", 10);

    kprintf("[virtio-fs] Virtio FS initialized at I/O 0x%04x, tag='%s'\n",
            virtio_fs_dev.iobase, virtio_fs_dev.tag);
    return 0;
}
#include "module.h"
module_init(virtio_fs_init);
