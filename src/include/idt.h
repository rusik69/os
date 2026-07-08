#ifndef IDT_H
#define IDT_H

#include "types.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed));

typedef void (*isr_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
void idt_register_handler(uint8_t vector, isr_handler_t handler);

/* Register an interrupt handler with a human-readable name (for /proc/interrupts).
 * If name is NULL, the vector name is left unchanged. */
void idt_register_handler_named(uint8_t vector, isr_handler_t handler, const char *name);

/* Update the IST field of an already-registered IDT entry */
void idt_set_gate_ist(int num, uint8_t ist);

/* ── IRQ allocator ────────────────────────────────────────────────── */
/* Allocate a contiguous range of IRQ vectors. Returns base vector or < 0. */
int irq_alloc_range(int count);

/* Free a previously allocated IRQ range. */
void irq_free_range(int base, int count);

/* ── Interrupt statistics (for /proc/interrupts) ──────────────────── */

/* Maximum number of CPU cores we track interrupt statistics for */
#define IDT_NR_CPUS  16

/* Number of interrupt vectors we track (0..255) */
#define IDT_NUM_VECTORS  256

/* Get the per-CPU per-vector interrupt count.  Returns 0 for invalid args. */
uint64_t idt_get_irq_count(int cpu, int vector);

/* Get the name associated with an interrupt vector, or NULL if unnamed. */
const char *idt_get_vector_name(int vector);

/* Set the name associated with an interrupt vector.  The string is not copied;
 * caller must keep it alive (static/rodata). */
void idt_set_vector_name(int vector, const char *name);

extern void idt_load(struct idt_pointer *ptr);

/* Common interrupt handler — called from assembly stubs (idt_asm.asm). */
void isr_common_handler(struct interrupt_frame *frame);

#endif /* IDT_H */
