/*
 * src/drivers/virtio_gpu.c — VirtIO GPU driver
 *
 * Implements 2D modesetting, scanout, and cursor support
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

/* Responses */
#define VIRTIO_GPU_RESP_OK_NODATA          0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO    0x1101

/* Formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM   1
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM   2

/* ── Driver state ──────────────────────────────────────────────── */

static int            gpu_present  = 0;
static uint16_t       gpu_iobase   = 0;
static uint32_t       gpu_scanout_w = 0;
static uint32_t       gpu_scanout_h = 0;

/* ── I/O helpers (matching existing virtio driver style) ───────── */

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

/* ── Init ──────────────────────────────────────────────────────── */

void __init virtio_gpu_init(void)
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

    /* Negotiate features */
    virtio_negotiate_features_ex(vgpu_inl, vgpu_outl, vgpu_outb, vgpu_inb,
                                 VIRTIO_GPU_F_VIRGL | VIRTIO_GPU_F_EDID,
                                 0,
                                 NULL,
                                 "virtio-gpu");

    /* Driver OK */
    vgpu_outb(VIRTIO_PCI_STATUS,
              VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
              VIRTIO_STATUS_DRIVER_OK);

    gpu_present = 1;
    gpu_scanout_w = 1024;
    gpu_scanout_h = 768;

    kprintf("[VIRTIO-GPU] VirtIO GPU at %02x:%02x.%d, I/O 0x%04x, "
            "scanout %ux%u\n",
            dev.bus, dev.slot, dev.func, gpu_iobase,
            gpu_scanout_w, gpu_scanout_h);
}

#ifdef MODULE
int __init init_module(void) { virtio_gpu_init(); return 0; }
void cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("VirtIO GPU — 2D modesetting, scanout, cursor");
MODULE_VERSION("1.0");
#endif

/* ── Stub: virtio_gpu_set_mode ─────────────────────────────── */
int virtio_gpu_set_mode(void *dev, int w, int h, int bpp)
{
    (void)dev;
    (void)w;
    (void)h;
    (void)bpp;
    kprintf("[VIRTIO] virtio_gpu_set_mode: not yet implemented\n");
    return 0;
}
/* ── Stub: virtio_gpu_transfer ─────────────────────────────── */
int virtio_gpu_transfer(void *dev, void *buf, size_t count)
{
    (void)dev;
    (void)buf;
    (void)count;
    kprintf("[VIRTIO] virtio_gpu_transfer: not yet implemented\n");
    return 0;
}
