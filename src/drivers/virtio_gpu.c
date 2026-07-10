/*
 * src/drivers/virtio_gpu.c — VirtIO GPU driver
 *
 * Implements 2D modesetting, scanout, cursor support,
 * 3D context/resource management, and blob resources
 * for the VirtIO GPU device (PCI vendor 0x1AF4, device 0x1050).
 * Follows existing virtio probe patterns (virtio_blk, virtio_net).
 */

#include "types.h"
#include "printf.h"
#include "string.h"
#include "io.h"
#include "pci.h"
#include "virtio.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "errno.h"
#include "drm_fence.h"

#ifdef MODULE
#include "module.h"
#endif

/* ── Device IDs ────────────────────────────────────────────────── */

#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_GPU_DEVICE   0x1050

/* ── Feature bits ──────────────────────────────────────────────── */

#define VIRTIO_GPU_F_VIRGL          (1u << 0)
#define VIRTIO_GPU_F_EDID          (1u << 1)
#define VIRTIO_GPU_F_RESOURCE_BLOB (1u << 2)
#define VIRTIO_GPU_F_CONTEXT_INIT  (1u << 3)

/* ── GPU control structures (virtio-gpu spec) ──────────────────── */

/* Commands */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO    0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D  0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF      0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT         0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH      0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_UPDATE_CURSOR       0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR         0x0301

/* 3D protocol commands */
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D  0x0108
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB 0x0109
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_V2  0x010A
#define VIRTIO_GPU_CMD_CTX_CREATE          0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY         0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define VIRTIO_GPU_CMD_SUBMIT_3D           0x0204
#define VIRTIO_GPU_CMD_FENCE_RETIRE        0x0206

/* ── Fence flags ──────────────────────────────────────────────── */
#define VIRTIO_GPU_FLAG_FENCE              (1u << 0)

/* Responses */
#define VIRTIO_GPU_RESP_OK_NODATA          0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO    0x1101
#define VIRTIO_GPU_RESP_OK_CREATE_3D       0x1102
#define VIRTIO_GPU_RESP_OK_CREATE_BLOB      0x1103
#define VIRTIO_GPU_RESP_OK_CONTEXT_CREATE  0x1104
#define VIRTIO_GPU_RESP_ERR_UNSPEC         0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY  0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1205

/* Formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM   1
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM   2

/* 3D target types (GL_TEXTURE_* constants) */
#define VIRTIO_GPU_GL_TARGET_TEXTURE_2D                    0x0DE1
#define VIRTIO_GPU_GL_TARGET_TEXTURE_3D                    0x806F
#define VIRTIO_GPU_GL_TARGET_TEXTURE_CUBE_MAP              0x8513
#define VIRTIO_GPU_GL_TARGET_TEXTURE_RECTANGLE             0x84F5
#define VIRTIO_GPU_GL_TARGET_TEXTURE_1D_ARRAY              0x8C18
#define VIRTIO_GPU_GL_TARGET_TEXTURE_2D_ARRAY              0x8C1A
#define VIRTIO_GPU_GL_TARGET_TEXTURE_BUFFER                0x8C2A
#define VIRTIO_GPU_GL_TARGET_TEXTURE_CUBE_MAP_ARRAY        0x9009
#define VIRTIO_GPU_GL_TARGET_TEXTURE_2D_MULTISAMPLE        0x9100
#define VIRTIO_GPU_GL_TARGET_TEXTURE_2D_MULTISAMPLE_ARRAY  0x9102

/* Blob resource constants */
#define VIRTIO_GPU_BLOB_MEM_GUEST           0x0001
#define VIRTIO_GPU_BLOB_MEM_HOST3D          0x0002
#define VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST    0x0003

#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE       (1u << 0)
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE      (1u << 1)
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE   (1u << 2)

/* ── Protocol structures ──────────────────────────────────────── */

#pragma pack(push, 1)

/* Generic control header */
struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

/* Resource attach backing: followed by nr_entries of virtio_gpu_mem_entry */
struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    /* struct virtio_gpu_mem_entry entries[0]; */
};

/* Resource detach backing */
struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

/* Single memory entry for backing storage */
struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

/* 3D resource create */
struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t level;
    uint32_t nr_samples;
    uint32_t flags;
};

/* Blob resource create */
struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t nr_entries;
    uint64_t blob_id;
    uint64_t size;
    /* struct virtio_gpu_mem_entry entries[0]; */
};

/* Context create */
struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t padding;
    uint8_t  name[64];
};

/* Simple response (for commands that return OK_NODATA or an error) */
struct virtio_gpu_resp_hdr {
    struct virtio_gpu_ctrl_hdr hdr;
};

/* Resource unref */
struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

/* Context attach/detach resource */
struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
};

#pragma pack(pop)

/* ── Internal resource tracking ───────────────────────────────── */

#define MAX_GPU_CONTEXTS    16
#define MAX_GPU_RESOURCES   256

struct gpu_context {
    uint32_t id;
    int      active;
    uint8_t  name[64];
    uint32_t name_len;
};

struct gpu_resource {
    uint32_t id;
    int      active;
    uint32_t target;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t level;
    uint32_t nr_samples;
    uint32_t bind;
    uint32_t flags;
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint64_t blob_id;
    uint64_t size;
    int      is_3d;
    int      is_blob;
    /* Number of backing entries */
    uint32_t nr_entries;
    /* Simple bitmap: which contexts have this attached */
    uint32_t ctx_attached;
};

/* ── Virtqueue for GPU commands ───────────────────────────────── */

#define VRING_GPU_SIZE          16
#define GPU_QUEUE_MEM_SIZE      4096
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VRING_GPU_SIZE];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[VRING_GPU_SIZE];
};

/* ── Command-response pair for synchronous submission ─────────── */

#define GPU_CMD_BUF_SIZE    512
#define GPU_RESP_BUF_SIZE   256

struct gpu_cmd_slot {
    uint8_t  cmd_buf[GPU_CMD_BUF_SIZE];
    uint8_t  resp_buf[GPU_RESP_BUF_SIZE];
    uint16_t last_used_idx;
    int      in_use;
};

/* ── Driver state ─────────────────────────────────────────────── */

static int            gpu_present  = 0;
static uint16_t       gpu_iobase   = 0;
static uint32_t       gpu_scanout_w = 0;
static uint32_t       gpu_scanout_h = 0;

/* Virtqueue memory */
static uint8_t gpu_queue_mem[GPU_QUEUE_MEM_SIZE] __attribute__((aligned(4096)));
static struct vring_desc  *gpu_descs;
static struct vring_avail *gpu_avail;
static struct vring_used  *gpu_used;
static uint16_t gpu_last_used_idx;

/* Command slots (each can have one in-flight command) */
static struct gpu_cmd_slot gpu_cmd_slot;

/* Context and resource tracking */
static struct gpu_context gpu_contexts[MAX_GPU_CONTEXTS];
static struct gpu_resource gpu_resources[MAX_GPU_RESOURCES];
static uint32_t gpu_next_ctx_id = 1;
static uint32_t gpu_next_resource_id = 1;

/* ── Fence tracking ────────────────────────────────────────────── */

#define MAX_GPU_FENCES    64

struct gpu_fence {
    uint32_t id;               /* Local fence tracking ID */
    int      active;           /* Slot in use */
    int      signaled;         /* 1 = fence has been signaled */
    uint32_t ctx_id;           /* Owning context (0 = global) */
    uint64_t fence_id;         /* Host-side fence identifier from device */
    uint64_t seqno;            /* Monotonically increasing sequence number */
    struct dma_fence *dma;     /* DRM dma_fence for external synchronization */
};

static struct gpu_fence gpu_fences[MAX_GPU_FENCES];
static uint32_t gpu_next_fence_id = 1;
static uint64_t gpu_fence_seqno = 0;

/* ── I/O helpers ──────────────────────────────────────────────── */

static inline void vgpu_outb(uint8_t off, uint8_t v)  { outb(gpu_iobase + off, v); }
static inline void vgpu_outw(uint8_t off, uint16_t v) { outw(gpu_iobase + off, v); }
static inline void vgpu_outl(uint8_t off, uint32_t v) {
    outb((uint16_t)(gpu_iobase + off),     (uint8_t)v);
    outb((uint16_t)(gpu_iobase + off + 1), (uint8_t)(v >> 8));
    outb((uint16_t)(gpu_iobase + off + 2), (uint8_t)(v >> 16));
    outb((uint16_t)(gpu_iobase + off + 3), (uint8_t)(v >> 24));
}
static inline uint8_t  vgpu_inb(uint8_t off)  { return inb(gpu_iobase + off); }
static inline uint16_t vgpu_inw(uint8_t off)  { return inw(gpu_iobase + off); }
static inline uint32_t vgpu_inl(uint8_t off) {
    return (uint32_t)inb((uint16_t)(gpu_iobase + off)) |
           ((uint32_t)inb((uint16_t)(gpu_iobase + off + 1)) << 8)  |
           ((uint32_t)inb((uint16_t)(gpu_iobase + off + 2)) << 16) |
           ((uint32_t)inb((uint16_t)(gpu_iobase + off + 3)) << 24);
}

/* ── Virtqueue helpers ────────────────────────────────────────── */

static void gpu_select_queue(uint16_t idx)
{
    vgpu_outw(VIRTIO_PCI_QUEUE_SEL, idx);
}

static int gpu_init_virtqueue(void)
{
    memset(gpu_queue_mem, 0, sizeof(gpu_queue_mem));

    /* Point the device at our queue memory */
    uint64_t phys = (uint64_t)(uintptr_t)gpu_queue_mem;
    vgpu_outl(VIRTIO_PCI_QUEUE_PFN, (uint32_t)(phys >> 12));

    /* Set up ring pointers into the queue memory */
    gpu_descs = (struct vring_desc *)gpu_queue_mem;
    gpu_avail = (struct vring_avail *)(gpu_queue_mem +
                  sizeof(struct vring_desc) * VRING_GPU_SIZE);
    gpu_used  = (struct vring_used  *)(gpu_queue_mem + 2048);

    gpu_last_used_idx = 0;
    memset(&gpu_cmd_slot, 0, sizeof(gpu_cmd_slot));

    return 0;
}

/* ── Synchronous command submission to the GPU ──────────────────
 * Builds a descriptor chain in the virtqueue:
 *   [0] command buffer (device-readable)
 *   [1] response buffer (device-writable)
 * Submits and busy-waits for completion.
 * Returns the response type on success, or a negative errno. */
static int gpu_send_cmd(const struct virtio_gpu_ctrl_hdr *hdr,
                         size_t extra_bytes, const void *extra_data,
                         struct virtio_gpu_ctrl_hdr *resp_out,
                         size_t resp_extra_len)
{
    size_t cmd_len;

    if (!gpu_present)
        return -ENODEV;

    /* Build the full command in the slot buffer */
    struct gpu_cmd_slot *slot = &gpu_cmd_slot;
    if (slot->in_use)
        return -EBUSY;

    cmd_len = sizeof(*hdr) + extra_bytes;
    if (cmd_len > GPU_CMD_BUF_SIZE)
        return -EINVAL;
    if (resp_extra_len + sizeof(*resp_out) > GPU_RESP_BUF_SIZE)
        return -EINVAL;

    memcpy(slot->cmd_buf, hdr, sizeof(*hdr));
    if (extra_bytes && extra_data)
        memcpy(slot->cmd_buf + sizeof(*hdr), extra_data, extra_bytes);

    slot->in_use = 1;

    uint16_t prev_used = gpu_used->idx;

    /* Build descriptor chain: [0] = cmd (device-read), [1] = resp (device-write) */
    struct vring_desc *desc = gpu_descs;

    desc[0].addr  = (uint64_t)(uintptr_t)slot->cmd_buf;
    desc[0].len   = (uint32_t)cmd_len;
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next  = 1;

    desc[1].addr  = (uint64_t)(uintptr_t)slot->resp_buf;
    desc[1].len   = (uint32_t)(sizeof(struct virtio_gpu_ctrl_hdr) + resp_extra_len);
    desc[1].flags = VRING_DESC_F_WRITE;
    desc[1].next  = 0;

    /* Submit to avail ring */
    uint16_t avail_idx = gpu_avail->idx & (VRING_GPU_SIZE - 1);
    gpu_avail->ring[avail_idx] = 0;  /* descriptor index 0 (head of chain) */
    __asm__ volatile("" ::: "memory");
    gpu_avail->idx++;
    __asm__ volatile("" ::: "memory");

    /* Notify device */
    gpu_select_queue(0);
    vgpu_outw(VIRTIO_PCI_QUEUE_NOTIFY, 0);

    /* Busy-wait for completion */
    uint32_t timeout = 100000;
    while (gpu_used->idx == prev_used && timeout--) {
        __asm__ volatile("pause");
    }

    if (timeout == 0) {
        kprintf("[VIRTIO-GPU] command timeout (type=0x%04X)\n",
                (unsigned int)hdr->type);
        slot->in_use = 0;
        return -ETIME;
    }

    /* Copy response */
    struct virtio_gpu_ctrl_hdr *resp = (struct virtio_gpu_ctrl_hdr *)slot->resp_buf;
    if (resp_out)
        memcpy(resp_out, resp, sizeof(*resp));

    uint32_t resp_type = resp->type;
    slot->in_use = 0;

    /* Check for errors */
    if (resp_type == VIRTIO_GPU_RESP_ERR_UNSPEC)
        return -EIO;
    if (resp_type == VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY)
        return -ENOMEM;
    if (resp_type == VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT)
        return -EINVAL;
    if (resp_type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID)
        return -ENOENT;
    if (resp_type == VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID)
        return -ENOENT;
    if (resp_type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER)
        return -EINVAL;

    return (int)resp_type;
}

/* ── Context tracking helpers ─────────────────────────────────── */

static struct gpu_context *gpu_find_context(uint32_t ctx_id)
{
    for (int i = 0; i < MAX_GPU_CONTEXTS; i++) {
        if (gpu_contexts[i].active && gpu_contexts[i].id == ctx_id)
            return &gpu_contexts[i];
    }
    return NULL;
}

static struct gpu_context *gpu_alloc_context(void)
{
    struct gpu_context *ctx = NULL;
    /* First, try to find an unused slot */
    for (int i = 0; i < MAX_GPU_CONTEXTS; i++) {
        if (!gpu_contexts[i].active) {
            ctx = &gpu_contexts[i];
            break;
        }
    }
    if (!ctx)
        return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->id = gpu_next_ctx_id++;
    ctx->active = 1;

    return ctx;
}

static void gpu_free_context(struct gpu_context *ctx)
{
    if (!ctx)
        return;
    ctx->active = 0;
}

/* ── Resource tracking helpers ────────────────────────────────── */

static struct gpu_resource *gpu_find_resource(uint32_t res_id)
{
    for (int i = 0; i < MAX_GPU_RESOURCES; i++) {
        if (gpu_resources[i].active && gpu_resources[i].id == res_id)
            return &gpu_resources[i];
    }
    return NULL;
}

static struct gpu_resource *gpu_alloc_resource(void)
{
    struct gpu_resource *res = NULL;
    for (int i = 0; i < MAX_GPU_RESOURCES; i++) {
        if (!gpu_resources[i].active) {
            res = &gpu_resources[i];
            break;
        }
    }
    if (!res)
        return NULL;

    memset(res, 0, sizeof(*res));
    res->id = gpu_next_resource_id++;
    res->active = 1;

    return res;
}

static void gpu_free_resource(struct gpu_resource *res)
{
    if (!res)
        return;
    res->active = 0;
}

/* ── Fence helpers ──────────────────────────────────────────────── */

static struct gpu_fence *gpu_find_fence(uint32_t id)
{
    for (int i = 0; i < MAX_GPU_FENCES; i++) {
        if (gpu_fences[i].active && gpu_fences[i].id == id)
            return &gpu_fences[i];
    }
    return NULL;
}

static struct gpu_fence *gpu_alloc_fence(void)
{
    struct gpu_fence *f = NULL;
    for (int i = 0; i < MAX_GPU_FENCES; i++) {
        if (!gpu_fences[i].active) {
            f = &gpu_fences[i];
            break;
        }
    }
    if (!f)
        return NULL;

    memset(f, 0, sizeof(*f));
    f->id = gpu_next_fence_id++;
    f->active = 1;
    f->seqno = ++gpu_fence_seqno;
    return f;
}

static void gpu_free_fence(struct gpu_fence *f)
{
    if (!f || !f->active)
        return;
    if (f->dma)
        dma_fence_put(f->dma);
    f->active = 0;
    f->dma = NULL;
}

/* After a command response is received, check whether the fence_id
 * in the response matches any pending fence and signal it. */
static int gpu_check_response_fence(const struct virtio_gpu_ctrl_hdr *resp)
{
    struct gpu_fence *f;
    int ret;

    if (!resp || resp->fence_id == 0 || !gpu_present)
        return -ENOENT;

    for (int i = 0; i < MAX_GPU_FENCES; i++) {
        if (!gpu_fences[i].active || gpu_fences[i].signaled)
            continue;
        if (gpu_fences[i].fence_id != resp->fence_id)
            continue;

        f = &gpu_fences[i];

        /* Determine fence status from response type */
        if (resp->type == VIRTIO_GPU_RESP_ERR_UNSPEC ||
            resp->type == VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY ||
            resp->type == VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT ||
            resp->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID ||
            resp->type == VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID ||
            resp->type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER) {
            /* Fence completed with error */
            f->signaled = 1;
            if (f->dma) {
                ret = dma_fence_signal_error(f->dma, -EIO);
                (void)ret;
            }
            return 1;
        }

        /* Normal completion — response type indicates success */
        f->signaled = 1;
        if (f->dma) {
            ret = dma_fence_signal(f->dma);
            (void)ret;
        }
        return 1;
    }
    return 0;
}

/* ── Public API: fence management ──────────────────────────────── */

static uint32_t virtio_gpu_fence_create(uint32_t ctx_id)
{
    struct gpu_fence *f;

    if (!gpu_present)
        return 0;

    f = gpu_alloc_fence();
    if (!f)
        return 0;

    f->ctx_id = ctx_id;
    f->signaled = 0;
    f->fence_id = 0;

    /* Create a DRM dma_fence for external synchronization */
    uint64_t dma_ctx = dma_fence_context_alloc();
    f->dma = dma_fence_create(dma_ctx, f->seqno);
    if (!f->dma) {
        kprintf("[VIRTIO-GPU] fence_create: dma_fence allocation failed\n");
    }

    kprintf("[VIRTIO-GPU] fence %u created (ctx=%u, seqno=%llu)\n",
            (unsigned int)f->id, (unsigned int)ctx_id,
            (unsigned long long)f->seqno);

    return f->id;
}

static int virtio_gpu_fence_destroy(uint32_t fence_id)
{
    struct gpu_fence *f;

    if (!gpu_present)
        return -ENODEV;

    f = gpu_find_fence(fence_id);
    if (!f)
        return -ENOENT;

    if (!f->signaled) {
        kprintf("[VIRTIO-GPU] fence_destroy: fence %u still pending, forcing\n",
                (unsigned int)fence_id);
    }

    gpu_free_fence(f);
    return 0;
}

static int virtio_gpu_fence_wait(uint32_t fence_id, uint64_t timeout_ms)
{
    struct gpu_fence *f;

    if (!gpu_present)
        return -ENODEV;

    f = gpu_find_fence(fence_id);
    if (!f)
        return -ENOENT;

    if (f->signaled)
        return 0;

    /* If we have a dma_fence, use its wait mechanism */
    if (f->dma) {
        if (timeout_ms == 0)
            return dma_fence_wait(f->dma);
        else
            return dma_fence_wait_timeout(f->dma, timeout_ms * 1000000ULL);
    }

    /* Fallback: busy-wait with timeout */
    uint32_t timeout = 10000000;
    while (!f->signaled && timeout--) {
        __asm__ volatile("pause");
    }

    if (timeout == 0) {
        kprintf("[VIRTIO-GPU] fence_wait(%u) timeout after %llu ms\n",
                (unsigned int)fence_id, (unsigned long long)timeout_ms);
        return -ETIME;
    }

    return 0;
}

static int virtio_gpu_fence_poll(uint32_t fence_id)
{
    struct gpu_fence *f;

    if (!gpu_present)
        return -ENODEV;

    f = gpu_find_fence(fence_id);
    if (!f)
        return -ENOENT;

    if (f->signaled) {
        /* If we have a dma_fence, also check its status */
        if (f->dma) {
            int status = dma_fence_get_status(f->dma);
            if (status < 0)
                return status;  /* Signaled with error */
        }
        return 1;  /* Signaled successfully */
    }

    /* Check the dma_fence directly */
    if (f->dma && dma_fence_is_signaled(f->dma)) {
        f->signaled = 1;
        return 1;
    }

    return 0;  /* Not yet signaled */
}

static int virtio_gpu_fence_signal(uint32_t fence_id)
{
    struct gpu_fence *f;

    if (!gpu_present)
        return -ENODEV;

    f = gpu_find_fence(fence_id);
    if (!f)
        return -ENOENT;

    if (f->signaled)
        return 0;  /* Already signaled, idempotent */

    f->signaled = 1;
    if (f->dma) {
        int ret = dma_fence_signal(f->dma);
        (void)ret;
    }

    return 0;
}

/* ── Public API: 3D submission with fence support ──────────────── */

static int virtio_gpu_submit_3d(uint32_t ctx_id,
                          const void *cmd_buf, uint32_t cmd_size,
                          uint32_t nr_resources,
                          const uint32_t *resource_ids,
                          uint32_t fence_id)
{
    struct virtio_gpu_ctrl_hdr cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_fence *f;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    if (!cmd_buf || cmd_size == 0)
        return -EINVAL;

    if (cmd_size > GPU_CMD_BUF_SIZE - sizeof(cmd))
        return -EINVAL;

    /* Look up fence if provided */
    if (fence_id != 0) {
        f = gpu_find_fence(fence_id);
        if (!f)
            return -ENOENT;
        if (f->signaled) {
            /* Reuse: reset signaled state for re-submission */
            f->signaled = 0;
        }
    } else {
        f = NULL;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.type  = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd.flags = (fence_id != 0) ? VIRTIO_GPU_FLAG_FENCE : 0;
    cmd.ctx_id = ctx_id;
    cmd.fence_id = fence_id;

    kprintf("[VIRTIO-GPU] submit_3d ctx=%u cmd_size=%u nr_res=%u",
            (unsigned int)ctx_id, (unsigned int)cmd_size,
            (unsigned int)nr_resources);
    if (fence_id != 0)
        kprintf(" fence=%u", (unsigned int)fence_id);
    kprintf("\n");

    /* Send command — the cmd_buf immediately follows the header */
    ret = gpu_send_cmd(&cmd, cmd_size, cmd_buf, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] submit_3d failed: %d\n", ret);
        return ret;
    }

    /* The device may have written back the fence_id in the response.
     * Check if this completes any pending fence. */
    if (fence_id != 0 && resp.fence_id != 0) {
        gpu_check_response_fence(&resp);
    }

    return 0;
}

/* ── Public API: context management ───────────────────────────── */

static int virtio_gpu_ctx_create(const char *name, uint32_t name_len)
{
    struct virtio_gpu_ctx_create cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_context *ctx;
    int ret;

    if (!gpu_present)
        return -ENODEV;
    if (!name || name_len == 0 || name_len > 64)
        return -EINVAL;

    ctx = gpu_alloc_context();
    if (!ctx)
        return -ENOMEM;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type  = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.flags = 0;
    cmd.hdr.ctx_id = ctx->id;
    cmd.nlen     = (uint32_t)name_len;
    cmd.padding  = 0;
    memcpy(cmd.name, name, name_len);

    ret = gpu_send_cmd(&cmd.hdr,
                        sizeof(cmd) - sizeof(cmd.hdr),
                        &cmd.nlen, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] ctx_create(0x%04X) failed: %d\n",
                (unsigned int)ctx->id, ret);
        gpu_free_context(ctx);
        return ret;
    }

    /* Save context info locally */
    ctx->name_len = (uint32_t)name_len;
    memcpy(ctx->name, name, name_len);

    kprintf("[VIRTIO-GPU] context 0x%04X created: \"%s\"\n",
            (unsigned int)ctx->id, name);

    return (int)ctx->id;
}

static int virtio_gpu_ctx_destroy(uint32_t ctx_id)
{
    struct virtio_gpu_ctrl_hdr cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_context *ctx;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    ctx = gpu_find_context(ctx_id);
    if (!ctx)
        return -ENOENT;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type  = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.flags = 0;
    cmd.ctx_id = ctx_id;

    ret = gpu_send_cmd(&cmd, 0, NULL, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] ctx_destroy(0x%04X) failed: %d\n",
                (unsigned int)ctx_id, ret);
        return ret;
    }

    /* Detach all resources from this context */
    for (int i = 0; i < MAX_GPU_RESOURCES; i++) {
        if (gpu_resources[i].active)
            gpu_resources[i].ctx_attached &= ~(1u << (ctx_id % 32));
    }

    gpu_free_context(ctx);

    kprintf("[VIRTIO-GPU] context 0x%04X destroyed\n",
            (unsigned int)ctx_id);

    return 0;
}

static int virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id)
{
    struct virtio_gpu_ctx_resource cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_context *ctx;
    struct gpu_resource *res;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    ctx = gpu_find_context(ctx_id);
    if (!ctx)
        return -ENOENT;

    res = gpu_find_resource(resource_id);
    if (!res)
        return -ENOENT;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type  = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    cmd.hdr.flags = 0;
    cmd.hdr.ctx_id = ctx_id;
    cmd.resource_id = resource_id;

    ret = gpu_send_cmd(&cmd.hdr, 0, NULL, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] ctx_attach_resource(0x%04X, 0x%04X) failed: %d\n",
                (unsigned int)ctx_id, (unsigned int)resource_id, ret);
        return ret;
    }

    res->ctx_attached |= (1u << (ctx_id % 32));

    return 0;
}

static int virtio_gpu_ctx_detach_resource(uint32_t ctx_id, uint32_t resource_id)
{
    struct virtio_gpu_ctx_resource cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_resource *res;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    res = gpu_find_resource(resource_id);
    if (!res)
        return -ENOENT;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type  = VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE;
    cmd.hdr.flags = 0;
    cmd.hdr.ctx_id = ctx_id;
    cmd.resource_id = resource_id;

    ret = gpu_send_cmd(&cmd.hdr, 0, NULL, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] ctx_detach_resource(0x%04X, 0x%04X) failed: %d\n",
                (unsigned int)ctx_id, (unsigned int)resource_id, ret);
        return ret;
    }

    res->ctx_attached &= ~(1u << (ctx_id % 32));

    return 0;
}

/* ── Public API: 3D resource management ───────────────────────── */

static int virtio_gpu_resource_create_3d(uint32_t resource_id,
                                   uint32_t target, uint32_t format,
                                   uint32_t bind,
                                   uint32_t width, uint32_t height,
                                   uint32_t depth, uint32_t array_size,
                                   uint32_t level, uint32_t nr_samples,
                                   uint32_t flags)
{
    struct virtio_gpu_resource_create_3d cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_resource *res;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    /* Check if this resource_id is already in use or allocate new */
    res = gpu_find_resource(resource_id);
    if (res)
        return -EEXIST;

    res = gpu_alloc_resource();
    if (!res)
        return -ENOMEM;

    /* Override with caller's resource_id if provided */
    if (resource_id != 0) {
        res->id = resource_id;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type  = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.hdr.flags = 0;
    cmd.resource_id = res->id;
    cmd.target    = target;
    cmd.format   = format;
    cmd.bind     = bind;
    cmd.width    = width;
    cmd.height   = height;
    cmd.depth    = depth;
    cmd.array_size = array_size;
    cmd.level    = level;
    cmd.nr_samples = nr_samples;
    cmd.flags    = flags;

    kprintf("[VIRTIO-GPU] create 3D resource 0x%04X: target=0x%04X "
            "fmt=0x%04X %ux%ux%u lvls=%u\n",
            (unsigned int)res->id, (unsigned int)target,
            (unsigned int)format, (unsigned int)width, (unsigned int)height,
            (unsigned int)depth, (unsigned int)level);

    ret = gpu_send_cmd(&cmd.hdr,
                        sizeof(cmd) - sizeof(cmd.hdr),
                        &cmd.resource_id, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] resource_create_3d(0x%04X) failed: %d\n",
                (unsigned int)res->id, ret);
        gpu_free_resource(res);
        return ret;
    }

    /* Populate tracking entry */
    res->is_3d       = 1;
    res->target      = target;
    res->format      = format;
    res->bind        = bind;
    res->width       = width;
    res->height      = height;
    res->depth       = depth;
    res->array_size  = array_size;
    res->level       = level;
    res->nr_samples  = nr_samples;
    res->flags       = flags;

    return (int)res->id;
}

static int virtio_gpu_resource_create_blob(uint32_t resource_id,
                                     uint32_t blob_mem, uint32_t blob_flags,
                                     uint64_t blob_id, uint64_t size)
{
    struct virtio_gpu_resource_create_blob cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_resource *res;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    /* Check if this resource_id is already in use or allocate new */
    res = gpu_find_resource(resource_id);
    if (res)
        return -EEXIST;

    res = gpu_alloc_resource();
    if (!res)
        return -ENOMEM;

    if (resource_id != 0)
        res->id = resource_id;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type  = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    cmd.hdr.flags = 0;
    cmd.resource_id = res->id;
    cmd.blob_mem  = blob_mem;
    cmd.blob_flags = blob_flags;
    cmd.blob_id   = blob_id;
    cmd.size      = size;
    cmd.nr_entries = 0;

    kprintf("[VIRTIO-GPU] create blob resource 0x%04X: mem=%u flags=0x%04X "
            "id=%llu size=%llu\n",
            (unsigned int)res->id, (unsigned int)blob_mem,
            (unsigned int)blob_flags, blob_id, size);

    ret = gpu_send_cmd(&cmd.hdr,
                        sizeof(cmd) - sizeof(cmd.hdr),
                        &cmd.resource_id, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] resource_create_blob(0x%04X) failed: %d\n",
                (unsigned int)res->id, ret);
        gpu_free_resource(res);
        return ret;
    }

    res->is_blob    = 1;
    res->blob_mem   = blob_mem;
    res->blob_flags = blob_flags;
    res->blob_id    = blob_id;
    res->size       = size;

    return (int)res->id;
}

static int virtio_gpu_resource_attach_backing(uint32_t resource_id,
                                        const struct virtio_gpu_mem_entry *entries,
                                        uint32_t nr_entries)
{
    /* Command layout:
     *   header + 8 bytes (resource_id + nr_entries) + entries[]
     */
    uint8_t stack_buf[512];
    struct virtio_gpu_ctrl_hdr *hdr;
    struct virtio_gpu_ctrl_hdr resp;
    struct virtio_gpu_resource_attach_backing *ab;
    struct gpu_resource *res;
    int ret;
    size_t total;

    if (!gpu_present)
        return -ENODEV;

    res = gpu_find_resource(resource_id);
    if (!res)
        return -ENOENT;

    total = sizeof(*ab) + nr_entries * sizeof(struct virtio_gpu_mem_entry);
    if (total > sizeof(stack_buf))
        return -EINVAL;

    memset(stack_buf, 0, sizeof(stack_buf));
    ab = (struct virtio_gpu_resource_attach_backing *)stack_buf;
    hdr = &ab->hdr;

    hdr->type  = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    hdr->flags = 0;
    ab->resource_id = resource_id;
    ab->nr_entries  = nr_entries;

    /* Copy entries after the fixed portion */
    if (nr_entries > 0 && entries) {
        struct virtio_gpu_mem_entry *dst =
            (struct virtio_gpu_mem_entry *)(stack_buf + sizeof(*ab));
        memcpy(dst, entries, nr_entries * sizeof(*entries));
    }

    ret = gpu_send_cmd(hdr,
                        total - sizeof(struct virtio_gpu_ctrl_hdr),
                        stack_buf + sizeof(struct virtio_gpu_ctrl_hdr),
                        &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] attach_backing(0x%04X) failed: %d\n",
                (unsigned int)resource_id, ret);
        return ret;
    }

    res->nr_entries = nr_entries;

    return 0;
}

static int virtio_gpu_resource_detach_backing(uint32_t resource_id)
{
    struct virtio_gpu_resource_detach_backing cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_resource *res;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    res = gpu_find_resource(resource_id);
    if (!res)
        return -ENOENT;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type  = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd.hdr.flags = 0;
    cmd.resource_id = resource_id;
    cmd.padding  = 0;

    ret = gpu_send_cmd(&cmd.hdr, 0, NULL, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] detach_backing(0x%04X) failed: %d\n",
                (unsigned int)resource_id, ret);
        return ret;
    }

    res->nr_entries = 0;

    return 0;
}

static int virtio_gpu_resource_unref(uint32_t resource_id)
{
    struct virtio_gpu_resource_unref cmd;
    struct virtio_gpu_ctrl_hdr resp;
    struct gpu_resource *res;
    int ret;

    if (!gpu_present)
        return -ENODEV;

    res = gpu_find_resource(resource_id);
    if (!res)
        return -ENOENT;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type  = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.hdr.flags = 0;
    cmd.resource_id = resource_id;
    cmd.padding  = 0;

    ret = gpu_send_cmd(&cmd.hdr, 0, NULL, &resp, 0);
    if (ret < 0) {
        kprintf("[VIRTIO-GPU] resource_unref(0x%04X) failed: %d\n",
                (unsigned int)resource_id, ret);
        return ret;
    }

    gpu_free_resource(res);

    return 0;
}

/* ── 2D-mode API (unchanged from original) ────────────────────── */

static int virtio_gpu_set_mode(void *dev, int w, int h, int bpp)
{
    (void)dev;
    (void)w;
    (void)h;
    (void)bpp;
    /* The GPU's scanout is configured via VIRTIO_GPU_CMD_SET_SCANOUT
     * and resource creation, which is handled at a higher layer.
     * For now, stub remains for 2D mode setting. */
    kprintf("[VIRTIO] virtio_gpu_set_mode: use virtio_gpu_2d API instead\n");
    return 0;
}

static int virtio_gpu_transfer(void *dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[VIRTIO] virtio_gpu_transfer: use virtio_gpu_2d API instead\n");
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────── */

static void __init virtio_gpu_init(void)
{
    struct pci_device dev;

    if (pci_find_device(VIRTIO_VENDOR, VIRTIO_GPU_DEVICE, &dev) < 0)
        return;

    gpu_iobase = (uint16_t)(dev.bar[0] & ~0x3u);
    if (!gpu_iobase) return;

    pci_enable_bus_master(&dev);

    /* Reset + acknowledge + driver */
    vgpu_outb(VIRTIO_PCI_STATUS, 0);
    vgpu_outb(VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Check queue size before feature negotiation (already probed PCI device) */
    gpu_select_queue(0);
    uint16_t qsz = vgpu_inw(VIRTIO_PCI_QUEUE_SIZE);
    if (qsz < VRING_GPU_SIZE) {
        kprintf("[VIRTIO-GPU] queue size %u < required %u\n",
                (unsigned int)qsz, (unsigned int)VRING_GPU_SIZE);
        return;
    }

    /* Negotiate features (request virgl + edid + blob + context_init) */
    virtio_negotiate_features_ex(vgpu_inl, vgpu_outl, vgpu_outb, vgpu_inb,
                                 VIRTIO_GPU_F_VIRGL |
                                 VIRTIO_GPU_F_EDID |
                                 VIRTIO_GPU_F_RESOURCE_BLOB |
                                 VIRTIO_GPU_F_CONTEXT_INIT,
                                 0,
                                 NULL,
                                 "virtio-gpu");

    /* Initialize the command virtqueue */
    if (gpu_init_virtqueue() < 0) {
        kprintf("[VIRTIO-GPU] failed to initialize virtqueue\n");
        return;
    }

    /* Driver OK */
    vgpu_outb(VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
              VIRTIO_STATUS_DRIVER_OK);

    gpu_present = 1;
    gpu_scanout_w = 1024;
    gpu_scanout_h = 768;

    /* Initialize context, resource, and fence tracking */
    memset(gpu_contexts, 0, sizeof(gpu_contexts));
    memset(gpu_resources, 0, sizeof(gpu_resources));
    memset(gpu_fences, 0, sizeof(gpu_fences));
    gpu_next_ctx_id = 1;
    gpu_next_resource_id = 1;
    gpu_next_fence_id = 1;
    gpu_fence_seqno = 0;

    kprintf("[VIRTIO-GPU] VirtIO GPU at %02x:%02x.%d, I/O 0x%04x, "
            "scanout %ux%u, queue=%u\n",
            dev.bus, dev.slot, dev.func, gpu_iobase,
            gpu_scanout_w, gpu_scanout_h,
            (unsigned int)qsz);
}

#ifdef MODULE
int __init init_module(void) { virtio_gpu_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO GPU — 2D/3D modesetting, scanout, cursor, 3D resource mgmt, fence sync");
MODULE_VERSION("1.0");
#endif
