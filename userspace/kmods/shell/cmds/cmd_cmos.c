/* cmd_cmos.c — read CMOS/NVRAM hardware configuration */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"

static uint8_t cmos_read(uint8_t reg) {
    return cmos_read_byte(reg & 0x7F);
}

void cmd_cmos(void) {
    uint8_t base_lo = cmos_read(0x15);
    uint8_t base_hi = cmos_read(0x16);
    uint8_t ext_lo  = cmos_read(0x17);
    uint8_t ext_hi  = cmos_read(0x18);
    uint8_t equip   = cmos_read(0x14);
    uint8_t floppy  = cmos_read(0x10);
    uint8_t hd      = cmos_read(0x12);
    uint8_t diag    = cmos_read(0x0E);

    uint16_t base_kb = ((uint16_t)base_hi << 8) | base_lo;
    uint16_t ext_kb  = ((uint16_t)ext_hi  << 8) | ext_lo;

    kprintf("CMOS hardware configuration:\n");
    kprintf("  Base memory:     %lu KB\n", (unsigned long)base_kb);
    kprintf("  Extended memory: %lu KB (%lu MB)\n",
            (unsigned long)ext_kb, (unsigned long)(ext_kb / 1024));
    kprintf("  Diagnostic:      0x%lx\n", (unsigned long)diag);
    kprintf("  Equipment flags: 0x%lx\n", (unsigned long)equip);
    kprintf("  Floppy type:     0x%lx\n", (unsigned long)floppy);
    kprintf("  HD type:         0x%lx\n", (unsigned long)hd);

    /* Decode equipment byte */
    if (equip & 0x01) {
        int nf = ((equip >> 6) & 3) + 1;
        kprintf("  Floppy drives:   %lu\n", (unsigned long)nf);
    } else {
        kprintf("  Floppy drives:   0\n");
    }

    uint8_t disp = (equip >> 4) & 3;
    const char *disp_str = "EGA/VGA";
    if (disp == 1) disp_str = "CGA 40x25";
    else if (disp == 2) disp_str = "CGA 80x25";
    else if (disp == 3) disp_str = "MDA monochrome";
    kprintf("  Display type:    %s\n", disp_str);

    if (equip & 0x02) kprintf("  Math co-processor: present\n");
    else              kprintf("  Math co-processor: absent\n");

    /* Extended memory via registers 0x30/0x31 (above 16MB boundary) */
    uint8_t xlo = cmos_read(0x30);
    uint8_t xhi = cmos_read(0x31);
    uint32_t xkb = ((uint32_t)xhi << 8) | xlo;
    if (xkb) kprintf("  High ext memory: %lu KB\n", (unsigned long)xkb);
}
