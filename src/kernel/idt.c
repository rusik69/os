#include "idt.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "export.h"
#include "smp.h"

static struct idt_entry idt[256];
static struct idt_pointer idt_ptr;
static isr_handler_t handlers[256];

/* ── Interrupt statistics for /proc/interrupts ───────────────────── */

/* Per-CPU per-vector interrupt counts.  Indexed as [cpu][vector]. */
static uint64_t irq_counts[IDT_NR_CPUS][IDT_NUM_VECTORS];

/* Human-readable names for interrupt vectors (static strings, not copied). */
static const char *vector_names[IDT_NUM_VECTORS];

uint64_t idt_get_irq_count(int cpu, int vector)
{
    if (cpu < 0 || cpu >= IDT_NR_CPUS) return 0;
    if (vector < 0 || vector >= IDT_NUM_VECTORS) return 0;
    return irq_counts[cpu][vector];
}

const char *idt_get_vector_name(int vector)
{
    if (vector < 0 || vector >= IDT_NUM_VECTORS) return NULL;
    return vector_names[vector];
}

void idt_set_vector_name(int vector, const char *name)
{
    if (vector < 0 || vector >= IDT_NUM_VECTORS) return;
    vector_names[vector] = name;
}

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);
extern void irq16(void);
extern void irq17(void);
extern void irq18(void);
extern void irq19(void);

static const char *exception_names[] = {
    "Division By Zero", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 FP Exception", "Alignment Check", "Machine Check", "SIMD FP Exception",
    "Virtualization Exception", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved"
};

static void idt_set_gate(uint8_t num, uint64_t handler, uint16_t sel, uint8_t type_attr) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].selector    = sel;
    idt[num].ist         = 0;
    idt[num].type_attr   = type_attr;
    idt[num].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[num].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[num].reserved    = 0;
}

void isr_common_handler(struct interrupt_frame *frame) {
    /* Count the interrupt — per-CPU, per-vector */
    {
        int cpu = (int)get_cpu_id();
        int vec = (int)frame->int_no;
        if (cpu >= 0 && cpu < IDT_NR_CPUS && vec >= 0 && vec < IDT_NUM_VECTORS) {
            irq_counts[cpu][vec]++;
        }
    }

    if (handlers[frame->int_no]) {
        handlers[frame->int_no](frame);
        return;
    }

    if (frame->int_no < 32) {
        kprintf("\n*** EXCEPTION: %s (#%lu) ***\n", exception_names[frame->int_no], (unsigned long)frame->int_no);
        kprintf("Error code: 0x%lx\n", (unsigned long)frame->error_code);
        kprintf("RIP: 0x%lx  RSP: 0x%lx\n", (unsigned long)frame->rip, (unsigned long)frame->rsp);
        kprintf("RAX: 0x%lx  RBX: 0x%lx  RCX: 0x%lx  RDX: 0x%lx\n",
                (unsigned long)frame->rax, (unsigned long)frame->rbx,
                (unsigned long)frame->rcx, (unsigned long)frame->rdx);
        cli();
        for (;;) hlt();
    }
}

void idt_register_handler(uint8_t vector, isr_handler_t handler) {
    idt_register_handler_named(vector, handler, NULL);
}

void idt_register_handler_named(uint8_t vector, isr_handler_t handler, const char *name) {
    handlers[vector] = handler;
    if (name != NULL) {
        vector_names[vector] = name;
    }
}

void idt_set_gate_ist(int num, uint8_t ist) {
    if (num >= 256) return;
    idt[num].ist = ist;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));
    memset(irq_counts, 0, sizeof(irq_counts));
    memset(vector_names, 0, sizeof(vector_names));

    /* Set default names for exception vectors */
    vector_names[0]  = "divide_error";
    vector_names[1]  = "debug";
    vector_names[2]  = "nmi";
    vector_names[3]  = "breakpoint";
    vector_names[4]  = "overflow";
    vector_names[5]  = "bounds";
    vector_names[6]  = "invalid_opcode";
    vector_names[7]  = "device_not_avail";
    vector_names[8]  = "double_fault";
    vector_names[9]  = "coprocessor_seg";
    vector_names[10] = "invalid_tss";
    vector_names[11] = "segment_not_present";
    vector_names[12] = "stack_segment";
    vector_names[13] = "general_protection";
    vector_names[14] = "page_fault";
    vector_names[16] = "x87_fp_exception";
    vector_names[17] = "alignment_check";
    vector_names[18] = "machine_check";
    vector_names[19] = "simd_fp_exception";
    vector_names[20] = "virtualization_exception";
    vector_names[30] = "security_exception";

    /* Default names for timer, keyboard, etc. (overridden by drivers) */
    vector_names[32] = "timer";
    vector_names[33] = "keyboard";
    vector_names[34] = "cascade";
    vector_names[35] = "com2";
    vector_names[36] = "com1";
    vector_names[37] = "lpt2";
    vector_names[38] = "floppy";
    vector_names[39] = "lpt1";
    vector_names[40] = "cmos_rtc";
    vector_names[41] = "perf";
    vector_names[42] = "acpi";
    vector_names[43] = "free";
    vector_names[44] = "ps2_mouse";
    vector_names[46] = "primary_ata";
    vector_names[47] = "secondary_ata";

    /* IPI vectors */
    vector_names[240] = "IPI-resched";
    vector_names[241] = "IPI-tlb-shoot";
    vector_names[242] = "IPI-backtrace";
    vector_names[243] = "IPI-membarrier";

    /* 0x8E = present, ring 0, interrupt gate */
    idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);

    /* IRQs: 32-47 */
    idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);

    /* IPI vectors: 240 (reschedule), 241 (TLB shootdown), 242 (backtrace), 243 (membarrier) */
    idt_set_gate(240, (uint64_t)irq16, 0x08, 0x8E);
    idt_set_gate(241, (uint64_t)irq17, 0x08, 0x8E);
    idt_set_gate(242, (uint64_t)irq18, 0x08, 0x8E);
    idt_set_gate(243, (uint64_t)irq19, 0x08, 0x8E);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;
    idt_load(&idt_ptr);
}

/* ── Exported symbols for driver modules ─────────────────────────── */
EXPORT_SYMBOL(idt_register_handler);
