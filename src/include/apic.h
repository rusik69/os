#ifndef APIC_H
#define APIC_H

#include "types.h"
#include "idt.h"

/* Local APIC MMIO offsets */
#define LAPIC_ID        0x020   /* APIC ID (RO) */
#define LAPIC_VER       0x030   /* Version (RO) */
#define LAPIC_TPR       0x080   /* Task Priority */
#define LAPIC_APR       0x090   /* Arbitration Priority (RO) */
#define LAPIC_PPR       0x0A0   /* Processor Priority (RO) */
#define LAPIC_EOI       0x0B0   /* End Of Interrupt (WO) */
#define LAPIC_LDR       0x0D0   /* Logical Destination */
#define LAPIC_DFR       0x0E0   /* Destination Format */
#define LAPIC_SVR       0x0F0   /* Spurious Vector */
#define LAPIC_ISR0      0x100   /* In-Service (bits 0..31) */
#define LAPIC_ISR1      0x110
#define LAPIC_ISR2      0x120
#define LAPIC_ISR3      0x130
#define LAPIC_TMR0      0x180
#define LAPIC_TMICT     0x380   /* Timer Initial Count */
#define LAPIC_TMCURR    0x390   /* Timer Current Count (RO) */
#define LAPIC_TMDIV     0x3E0   /* Timer Divide Config */
#define LAPIC_LVT_TIMER 0x320   /* Timer LVT */
#define LAPIC_LVT_PC    0x340   /* Performance Counter */
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_ICR_LOW   0x300   /* Interrupt Command (low) */
#define LAPIC_ICR_HIGH  0x310   /* Interrupt Command (high) */

/* Spurious Vector Register bits */
#define SVR_ENABLE      (1U << 8)
#define SVR_FOCUS_DIS   (1U << 9)

/* Timer LVT bits */
#define TIMER_MASKED    (1U << 16)
#define TIMER_PERIODIC  (1U << 17)
#define TIMER_ONESHOT   (0 << 17)

/* ICR bits */
#define ICR_INIT        (5 << 8)
#define ICR_STARTUP     (6 << 8)
#define ICR_DELIVERY    (1U << 12)
#define ICR_LEVEL       (1U << 15)
#define ICR_TRIGGER     (1U << 15)
#define ICR_ALL_EXCL    (0x80000)  /* All excluding self */
#define ICR_ALL_INCL    (0x80000 | 0x10000)  /* All including self */
#define ICR_DEST_FIXED  0

/* IPI vector numbers */
#define IPI_VECTOR_RESCHEDULE   0xF0
#define IPI_VECTOR_TLB_SHOOT    0xF1
#define IPI_VECTOR_BACKTRACE    0xF2
#define IPI_VECTOR_MEMBARRIER   0xF3
#define IPI_VECTOR_PANIC_HALT   0xF4
#define IPI_VECTOR_STOP_MACHINE 0xF5

/* I/O APIC registers */
#define IOAPIC_INDEX   0x00
#define IOAPIC_DATA    0x10
#define IOAPIC_ID      0x00
#define IOAPIC_VER     0x01
#define IOAPIC_ARB     0x02
#define IOAPIC_REDTBL  0x10   /* first redirection table entry */

/* I/O APIC redirection entry bits */
#define IOAPIC_MASKED  (1U << 16)

/* APIC virtual base addresses */
#define LAPIC_BASE_VIRT     0xFFFF8000FFE00000ULL
#define IOAPIC_BASE_VIRT    0xFFFF8000FEC00000ULL

void apic_init_local(void);
void apic_eoi(void);
uint32_t apic_read(uint32_t reg);
void apic_write(uint32_t reg, uint32_t val);
uint32_t apic_get_id(void);
void apic_send_ipi(uint32_t apic_id, uint32_t vector);
void apic_send_ipi_all_except(uint32_t vector);
void apic_send_init_ipi(uint32_t apic_id);
void apic_send_startup_ipi(uint32_t apic_id, uint32_t page_num);
int apic_is_init_complete(void);

/* I/O APIC */
void ioapic_init(void);
void ioapic_redirect_extint(uint8_t irq);
void ioapic_redirect_irq(uint8_t irq, uint8_t vector, uint32_t apic_id);
void ioapic_redirect_irq_level(uint8_t irq, uint8_t vector, uint32_t apic_id, int active_low);
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);
void ioapic_set_irq_destination(uint8_t irq, uint32_t apic_id);
void irq_ack(uint8_t irq);

/* IPI handlers */
void ipi_init(void);
void ipi_reschedule_handler(struct interrupt_frame *frame);
void ipi_tlb_shootdown_handler(struct interrupt_frame *frame);
void ipi_backtrace_handler(struct interrupt_frame *frame);
void ipi_membarrier_handler(struct interrupt_frame *frame);
void ipi_panic_halt_handler(struct interrupt_frame *frame);

/* APIC timer calibration (returns bus frequency in Hz) */
uint32_t apic_timer_calibrate(void);

#endif
