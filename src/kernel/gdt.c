#include "gdt.h"
#include "string.h"

#define GDT_ENTRIES 7

static struct gdt_entry gdt[GDT_ENTRIES];
static struct tss kernel_tss;
static struct gdt_pointer gdt_ptr;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity  = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void gdt_init(void) {
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code: index 1, selector 0x08 */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0); /* present, exec, read, 64-bit, 4K granularity */

    /* Kernel data: index 2, selector 0x10 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0); /* present, data, write, 4K granularity */

    /* User code: index 3, selector 0x18 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA0); /* present, DPL=3, exec, read, 64-bit */

    /* User data: index 4, selector 0x20 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xC0); /* present, DPL=3, data, write */

    /* TSS: index 5 (and 6 for upper base), selector 0x28 */
    memset(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.iopb_offset = sizeof(struct tss);

    uint64_t tss_base = (uint64_t)&kernel_tss;
    uint32_t tss_limit = sizeof(struct tss) - 1;

    gdt_set_entry(5, (uint32_t)(tss_base & 0xFFFFFFFF), tss_limit, 0x89, 0x00);

    /* TSS high half (entry 6 holds upper 32 bits of base) */
    struct gdt_entry *tss_high = &gdt[6];
    uint32_t tss_high_raw[2];
    tss_high_raw[0] = (uint32_t)(tss_base >> 32);
    tss_high_raw[1] = 0;
    __builtin_memcpy(tss_high, tss_high_raw, sizeof(tss_high_raw));

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    gdt_load(&gdt_ptr, 0x08, 0x10);
    tss_load(0x28);
}

void tss_set_rsp0(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}
