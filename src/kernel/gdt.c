#include "gdt.h"
#include "string.h"
#include "pmm.h"
#include "printf.h"
#include "idt.h"

#define GDT_ENTRIES 7

static struct gdt_entry gdt[GDT_ENTRIES];
static struct tss kernel_tss;
static struct gdt_pointer gdt_ptr;

/*
 * Early IST1 stack for double-fault protection before PMM is online.
 *
 * During GDT + IDT init (before pmm_init() + ist_init()), the TSS IST1
 * field is otherwise 0 — any #DF in that window would switch RSP to NULL
 * and triple-fault immediately.  This BSS-resident buffer gives #DF a
 * minimal safe stack that is replaced once the PMM-backed IST stack is
 * allocated in ist_init().
 *
 * 8 KB is enough for the push-all-registers frame (~120 bytes) plus the
 * double_fault_handler() C call chain.  ist_init() will replace ist1
 * with the larger (16 KB) PMM-allocated stack.
 */
#define EARLY_IST1_STACK_SIZE  8192
static uint8_t __attribute__((aligned(16))) early_ist1_stack[EARLY_IST1_STACK_SIZE];

static void gdt_set_entry(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity  = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void __init gdt_init(void) {
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code: index 1, selector 0x08 */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0); /* present, exec, read, 64-bit, 4K granularity */

    /* Kernel data: index 2, selector 0x10 */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0); /* present, data, write, 4K granularity */

    /* User data: index 3, selector 0x18
     * SYSRETQ: SS.SEL = (STAR[63:48]+8) | 3 = (0x10+8) | 3 = 0x1B = index 3 | RPL3
     * Must be a data segment (writeable, DPL=3) */
    gdt_set_entry(3, 0, 0xFFFFF, 0xF2, 0xC0); /* present, DPL=3, data, write */

    /* User code: index 4, selector 0x20
     * SYSRETQ: CS.SEL = STAR[63:48] + 16 = 0x10 + 16 = 0x20 = index 4
     * Must be a code segment (exec/read, DPL=3) */
    gdt_set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0); /* present, DPL=3, exec, read, 64-bit */

    /* TSS: index 5 (and 6 for upper base), selector 0x28 */
    memset(&kernel_tss, 0, sizeof(kernel_tss));
    kernel_tss.iopb_offset = sizeof(struct tss);

    /*
     * Install the early BSS-resident IST1 stack for double-fault
     * protection.  This is critical — without it, any #DF during the
     * boot window before ist_init() would see IST1 = NULL, switch RSP
     * to address 0, and triple-fault before printing anything.
     *
     * ist_init() will replace this with a larger PMM-allocated stack
     * once the physical memory manager is online.
     */
    kernel_tss.ist1 = (uint64_t)&early_ist1_stack[EARLY_IST1_STACK_SIZE];

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

/* ── IST stack management ────────────────────────────────────────── */

static uint64_t ist_stack_phys[4]; /* physical base of IST stacks (index 0 unused) */

void tss_set_ist(int index, uint64_t stack_top) {
    if (index < 1 || index > 7) return;
    switch (index) {
        case 1: kernel_tss.ist1 = stack_top; break;
        case 2: kernel_tss.ist2 = stack_top; break;
        case 3: kernel_tss.ist3 = stack_top; break;
        case 4: kernel_tss.ist4 = stack_top; break;
        case 5: kernel_tss.ist5 = stack_top; break;
        case 6: kernel_tss.ist6 = stack_top; break;
        case 7: kernel_tss.ist7 = stack_top; break;
        default: break;
    }
}

void __init ist_init(void) {
    /* ── Double fault IST stack (4 pages = 16 KB) ── */
    uint64_t df_phys = (uint64_t)(uintptr_t)pmm_alloc_frames(4);
    if (!df_phys) {
        kprintf("[!!] IST: failed to allocate double-fault stack\n");
        return;
    }
    kernel_tss.ist1 = (uint64_t)PHYS_TO_VIRT(df_phys) + 4 * PAGE_SIZE;
    ist_stack_phys[1] = df_phys;
    kprintf("[OK] IST1 (DF)  virt=0x%llx phys=0x%llx\n",
            (unsigned long long)kernel_tss.ist1,
            (unsigned long long)df_phys);

    /* ── NMI IST stack (2 pages = 8 KB) ── */
    uint64_t nmi_phys = (uint64_t)(uintptr_t)pmm_alloc_frames(2);
    if (!nmi_phys) {
        kprintf("[!!] IST: failed to allocate NMI stack\n");
        return;
    }
    kernel_tss.ist2 = (uint64_t)PHYS_TO_VIRT(nmi_phys) + 2 * PAGE_SIZE;
    ist_stack_phys[2] = nmi_phys;
    kprintf("[OK] IST2 (NMI) virt=0x%llx phys=0x%llx\n",
            (unsigned long long)kernel_tss.ist2,
            (unsigned long long)nmi_phys);

    /* ── Machine Check IST stack (2 pages = 8 KB) ── */
    uint64_t mce_phys = (uint64_t)(uintptr_t)pmm_alloc_frames(2);
    if (!mce_phys) {
        kprintf("[!!] IST: failed to allocate MCE stack\n");
        return;
    }
    kernel_tss.ist3 = (uint64_t)PHYS_TO_VIRT(mce_phys) + 2 * PAGE_SIZE;
    ist_stack_phys[3] = mce_phys;
    kprintf("[OK] IST3 (MCE) virt=0x%llx phys=0x%llx\n",
            (unsigned long long)kernel_tss.ist3,
            (unsigned long long)mce_phys);

    /* Patch IDT entries to use IST entries */
    idt_set_gate_ist(8, IST_INDEX_DF);   /* Double fault  → IST1 */
    idt_set_gate_ist(2, IST_INDEX_NMI);  /* NMI           → IST2 */
    idt_set_gate_ist(18, IST_INDEX_MCE); /* Machine Check → IST3 */

    kprintf("[OK] IST stacks configured for DF/NMI/MCE protection\n");
}

/* ── Stub: gdt_get_entry ─────────────────────────────── */
static int gdt_get_entry(int idx, void *entry)
{
    (void)idx;
    (void)entry;
    kprintf("[gdt] gdt_get_entry: not yet implemented\n");
    return 0;
}
