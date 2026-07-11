#define KERNEL_INTERNAL
#include "bochs.h"
#include "printf.h"
#include "types.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "err.h"
#define VBE_DISPI_INDEX_ID 0
#define VBE_DISPI_INDEX_XRES 1
#define VBE_DISPI_INDEX_YRES 2
#define VBE_DISPI_INDEX_BPP 3
#define VBE_DISPI_INDEX_ENABLE 4
#define VBE_DISPI_INDEX_BANK 5
#define VBE_DISPI_INDEX_VIRT_WIDTH 6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET 8
#define VBE_DISPI_INDEX_Y_OFFSET 9
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA 0x01CF
#define VBE_DISPI_LFB_ENABLED 0x40

/* Default LFB physical address when PCI probing fails (ISA/VGA legacy). */
#define VBE_DISPI_LFB_PHYS_DEFAULT  0xE0000000ULL
#define BOCHS_LFB_SIZE_DEFAULT      (16 * 1024 * 1024)  /* 16 MB */

static int bochs_present = 0;
static int bochs_width = 1024;
static int bochs_height = 768;
static uint64_t bochs_fb_phys = 0;
static uint32_t bochs_fb_size = 0;
static volatile uint8_t *bochs_fb_base = NULL;

/* -----------------------------------------------------------------
 * Probe the LFB physical address from the Bochs VGA PCI device
 * (vendor=0x1234, device=0x1111).  If the device isn't found or
 * BAR0 is an I/O port, fall back to the well-known ISA address.
 * ----------------------------------------------------------------- */
static uint64_t bochs_get_lfb_phys(void)
{
    struct pci_device dev;

    if (pci_find_device(0x1234, 0x1111, &dev) != 0)
        return VBE_DISPI_LFB_PHYS_DEFAULT;

    /* Enable PCI memory space so BAR reads are valid */
    uint32_t cmd = pci_read(dev.bus, dev.slot, dev.func, 0x04);
    pci_write(dev.bus, dev.slot, dev.func, 0x04, cmd | (1u << 1));

    uint32_t bar0 = pci_read(dev.bus, dev.slot, dev.func, 0x10);
    uint32_t bar1 = pci_read(dev.bus, dev.slot, dev.func, 0x14);

    uint64_t addr;

    /* Bit 0 = 1 ⇒ I/O BAR, not a memory-mappable framebuffer */
    if (bar0 & 1)
        return VBE_DISPI_LFB_PHYS_DEFAULT;
    /* Bit 2 = 1 ⇒ 64-bit BAR (bar0 + bar1) */
    if (bar0 & 0x4)
        addr = ((uint64_t)bar1 << 32) | (bar0 & 0xFFFFFFF0ULL);
    else
        addr = bar0 & 0xFFFFFFF0ULL;

    return addr ? addr : VBE_DISPI_LFB_PHYS_DEFAULT;
}

void bochs_init(void)
{
    /* Probe Bochs VBE */
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    uint16_t id = inw(VBE_DISPI_IOPORT_DATA);
    if (id == 0xB0C0 || id >= 0xB0C4) {
        bochs_present = 1;
        kprintf("[OK] Bochs VBE detected (ID=0x%x)\n", (uint32_t)id);
    } else {
        kprintf("[--] No Bochs VBE found\n");
        return;
    }

    /* Discover the LFB physical address via PCI */
    bochs_fb_phys = bochs_get_lfb_phys();
    bochs_fb_size = BOCHS_LFB_SIZE_DEFAULT;

    kprintf("[BOCHS] LFB physical address: 0x%llx (size %u)\n",
            (unsigned long long)bochs_fb_phys, bochs_fb_size);
}

int bochs_is_present(void) { return bochs_present; }

int bochs_get_width(void) { return bochs_width; }

int bochs_get_height(void) { return bochs_height; }

volatile uint8_t *bochs_get_fb_base(void) { return bochs_fb_base; }

uint64_t bochs_get_fb_phys(void) { return bochs_fb_phys; }

uint32_t bochs_get_fb_size(void) { return bochs_fb_size; }

int bochs_set_mode(int w, int h, int bpp)
{
    if (!bochs_present)
        return -1;

    /* Disable display while changing mode */
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_IOPORT_DATA, 0);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_XRES);
    outw(VBE_DISPI_IOPORT_DATA, (uint16_t)w);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_YRES);
    outw(VBE_DISPI_IOPORT_DATA, (uint16_t)h);

    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_BPP);
    outw(VBE_DISPI_IOPORT_DATA, (uint16_t)bpp);

    /* Enable display + LFB */
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    outw(VBE_DISPI_IOPORT_DATA, (uint16_t)(VBE_DISPI_LFB_ENABLED | 1));

    /* Read back enable register to verify mode was accepted */
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ENABLE);
    uint16_t enable = inw(VBE_DISPI_IOPORT_DATA);
    if (!(enable & VBE_DISPI_LFB_ENABLED)) {
        kprintf("[BOCHS] set_mode(%dx%d@%d): LFB not enabled (enable=0x%x)\n",
                w, h, bpp, (unsigned)enable);
        return -1;
    }

    /* Update cached dimensions */
    bochs_width  = w;
    bochs_height = h;

    /* Re-discover the LFB physical address (BAR may be invalid before enable) */
    if (!bochs_fb_phys)
        bochs_fb_phys = bochs_get_lfb_phys();
    bochs_fb_size = BOCHS_LFB_SIZE_DEFAULT;

    /* Map framebuffer into kernel virtual address space */
    uint64_t fb_sz = (uint64_t)w * (uint64_t)h * (uint64_t)(bpp / 8);
    if (fb_sz > bochs_fb_size)
        fb_sz = bochs_fb_size;

    bochs_fb_base = (volatile uint8_t *)vmm_map_phys(
        bochs_fb_phys, fb_sz,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);

    if (IS_ERR((const void *)(uintptr_t)bochs_fb_base)) {
        kprintf("[BOCHS] set_mode: framebuffer mapping FAILED "
                "(phys=0x%llx, size=%llu)\n",
                (unsigned long long)bochs_fb_phys,
                (unsigned long long)fb_sz);
        bochs_fb_base = NULL;
        return -1;
    }

    kprintf("[BOCHS] set_mode: %dx%d@%dbpp — framebuffer mapped "
            "(phys=0x%llx virt=%p size=%llu)\n",
            w, h, bpp,
            (unsigned long long)bochs_fb_phys,
            (const void *)(uintptr_t)bochs_fb_base,
            (unsigned long long)fb_sz);

    return 0;
}

#include "module.h"
module_init(bochs_init);

/* ── Stub: bochs_set_palette ─────────────────────────────── */
static int bochs_set_palette(const void *palette, int count)
{
    (void)palette;
    (void)count;
    kprintf("[BOCHS] bochs_set_palette: not yet implemented\n");
    return 0;
}
