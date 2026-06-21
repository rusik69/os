#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "bochs.h"
#include "pci.h"
#include "io.h"
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
static int bochs_present = 0;
static int bochs_width = 1024;
static int bochs_height = 768;
void bochs_init(void) {
    /* Probe Bochs VBE */
    outw(VBE_DISPI_INDEX_ID, VBE_DISPI_IOPORT_INDEX);
    uint16_t id = inw(VBE_DISPI_IOPORT_DATA);
    if (id == 0xB0C0 || id >= 0xB0C4) {
        bochs_present = 1;
        kprintf("[OK] Bochs VBE detected (ID=0x%x)\n", (uint32_t)id);
    } else {
        kprintf("[--] No Bochs VBE found\n");
    }
}
int bochs_is_present(void) { return bochs_present; }
int bochs_get_width(void) { return bochs_width; }
int bochs_get_height(void) { return bochs_height; }
void bochs_set_mode(int w, int h, int bpp) {
    if (!bochs_present) return;
    outw(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_IOPORT_INDEX);
    outw(0, VBE_DISPI_IOPORT_DATA);
    outw(VBE_DISPI_INDEX_XRES, VBE_DISPI_IOPORT_INDEX);
    outw((uint16_t)w, VBE_DISPI_IOPORT_DATA);
    outw(VBE_DISPI_INDEX_YRES, VBE_DISPI_IOPORT_INDEX);
    outw((uint16_t)h, VBE_DISPI_IOPORT_DATA);
    outw(VBE_DISPI_INDEX_BPP, VBE_DISPI_IOPORT_INDEX);
    outw((uint16_t)bpp, VBE_DISPI_IOPORT_DATA);
    outw(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_IOPORT_INDEX);
    outw((uint16_t)(VBE_DISPI_LFB_ENABLED | 1), VBE_DISPI_IOPORT_DATA);
}
#include "module.h"
module_init(bochs_init);

/* ── Stub: bochs_set_palette ─────────────────────────────── */
int bochs_set_palette(const void *palette, int count)
{
    (void)palette;
    (void)count;
    kprintf("[bochs] bochs_set_palette: not yet implemented\n");
    return 0;
}
