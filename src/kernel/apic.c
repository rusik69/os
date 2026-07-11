/* apic.c — Local APIC and I/O APIC driver */

#include "apic.h"
#include "pic.h"
#include "io.h"
#include "printf.h"
#include "timer.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "stop_machine.h"
#include "export.h"
#include "idt.h"
#include "smp.h"
#include "panic.h"
#include "process.h"
#include "scheduler.h"
#include "cpuhp.h"

/* ── Local APIC MMIO access ────────────────────────────────────────── */
/* Map the LAPIC at a fixed high-half virtual address */
static volatile uint32_t *lapic = NULL;

static void lapic_map(void) {
    if (!lapic) {
        /* Map the LAPIC physical page at the fixed virtual address */
        uint64_t lapic_phys = 0xFEE00000ULL;
        vmm_map_page(LAPIC_BASE_VIRT, lapic_phys,
                            VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);
        lapic = (volatile uint32_t *)LAPIC_BASE_VIRT;
    }
}

uint32_t apic_read(uint32_t reg) {
    return lapic[reg / 4];
}

void apic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
}

uint32_t apic_get_id(void) {
    return (apic_read(LAPIC_ID) >> 24) & 0xFF;
}

/* ── Local APIC initialization ────────────────────────────────────── */

static int apic_initialized = 0;

int apic_is_init_complete(void) {
    return apic_initialized;
}

void apic_eoi(void) {
    apic_write(LAPIC_EOI, 0);
}

void apic_init_local(void) {
    /* Enable the local APIC via MSR */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1B)); /* IA32_APIC_BASE */
    lo |= 0x800;  /* bit 11 = enable */
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x1B));

    lapic_map();

    /* Software enable: set spurious vector to 0xFF, enable bit */
    apic_write(LAPIC_SVR, 0xFF | SVR_ENABLE | SVR_FOCUS_DIS);

    /* Mask all LVT entries */
    apic_write(LAPIC_LVT_TIMER, TIMER_MASKED);
    apic_write(LAPIC_LVT_PC,    TIMER_MASKED);
    /* LINT0 must be unmasked with ExtINT delivery for the legacy PIC
     * (PIT, keyboard, RTC) to work.  It will be properly configured
     * when timer_init calls apic_enable_extint(). */
    apic_write(LAPIC_LVT_LINT0, TIMER_MASKED | (7 << 8)); /* masked + ExtINT */
    apic_write(LAPIC_LVT_LINT1, TIMER_MASKED);
    apic_write(LAPIC_LVT_ERROR, TIMER_MASKED);

    /* Set task priority to accept all interrupts */
    apic_write(LAPIC_TPR, 0);

    /* Acknowledge any pending interrupt */
    apic_write(LAPIC_EOI, 0);

    /* Configure destination format (flat model) */
    apic_write(LAPIC_DFR, 0xFFFFFFFF);

    /* Set logical destination to CPU ID */
    uint32_t apic_id = apic_get_id();
    apic_write(LAPIC_LDR, (1U << (apic_id & 0x0F)) << 24);

    apic_initialized = 1;

    kprintf("[OK] Local APIC (ID=%u) initialized\n", (unsigned int)apic_id);
}

/* ── IPI (Inter-Processor Interrupts) ─────────────────────────────── */

void apic_send_ipi(uint32_t apic_id, uint32_t vector) {
    apic_write(LAPIC_ICR_HIGH, apic_id << 24);
    apic_write(LAPIC_ICR_LOW,  vector | ICR_DEST_FIXED);
    /* Wait for delivery to complete */
    while (apic_read(LAPIC_ICR_LOW) & ICR_DELIVERY)
        __asm__ volatile("pause");
}

void apic_send_ipi_all_except(uint32_t vector) {
    apic_write(LAPIC_ICR_HIGH, 0);
    apic_write(LAPIC_ICR_LOW, vector | ICR_ALL_EXCL);
    while (apic_read(LAPIC_ICR_LOW) & ICR_DELIVERY)
        __asm__ volatile("pause");
}

void apic_send_init_ipi(uint32_t apic_id) {
    apic_write(LAPIC_ICR_HIGH, apic_id << 24);
    apic_write(LAPIC_ICR_LOW,  ICR_INIT | ICR_LEVEL);
    /* Wait for delivery */
    while (apic_read(LAPIC_ICR_LOW) & ICR_DELIVERY)
        __asm__ volatile("pause");
    /* 10 ms delay per MP spec */
    for (volatile int i = 0; i < 10000000; i++) __asm__ volatile("pause");
}

void apic_send_startup_ipi(uint32_t apic_id, uint32_t page_num) {
    apic_write(LAPIC_ICR_HIGH, apic_id << 24);
    apic_write(LAPIC_ICR_LOW,  ICR_STARTUP | (page_num & 0xFF));
    while (apic_read(LAPIC_ICR_LOW) & ICR_DELIVERY)
        __asm__ volatile("pause");
    /* Per MP spec, 200 µs delay between SIPIs */
    for (volatile int i = 0; i < 200000; i++) __asm__ volatile("pause");
}

/* ── APIC timer calibration ──────────────────────────────────────────
 * Uses the PIT (channel 2, gate high) to measure the APIC bus frequency.
 * We set the PIT to count down from a known value, measure how many
 * APIC timer ticks elapse in that period.
 */

#define PIT_CH2   0x42
#define PIT_CMD   0x43
#define PIT_CH0   0x40
#define PIT_GATE  0x61

uint32_t apic_timer_calibrate(void) {
    /* Set PIT channel 2 to one-shot mode, maximum count */
    outb(PIT_CMD, 0xB2);          /* channel 2, lobyte/hibyte, rate gen */
    outb(PIT_CH2, 0xFF);          /* low byte of max divisor */
    outb(PIT_CH2, 0xFF);          /* high byte */

    /* Enable PIT channel 2 gate and speaker (to let it count) */
    uint8_t gate = inb(PIT_GATE);
    outb(PIT_GATE, gate | 1);     /* gate high */

    /* Start APIC timer in one-shot mode with max initial count */
    apic_write(LAPIC_TMDIV, 0);   /* divide by 1 */
    apic_write(LAPIC_TMICT, 0xFFFFFFFF);

    /* Wait for PIT to reach 0 (one 1193180/65536 ≈ 18.2 Hz period) */
    for (;;) {
        /* Read PIT status: latch count */
        outb(PIT_CMD, 0xE2);      /* latch channel 2 */
        uint8_t lo = inb(PIT_CH2);
        uint8_t hi = inb(PIT_CH2);
        uint16_t count = lo | (hi << 8);
        if (count == 0) break;
    }

    /* Stop APIC timer */
    uint32_t ticks = 0xFFFFFFFF - apic_read(LAPIC_TMCURR);
    apic_write(LAPIC_LVT_TIMER, TIMER_MASKED);
    apic_write(LAPIC_TMICT, 0);

    /* Restore PIT gate */
    outb(PIT_GATE, gate & ~1);

    /* Re-program PIT channel 0 to 100 Hz (restore timer) */
    uint16_t divisor = 1193180 / TIMER_FREQ;
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

    kprintf("[OK] APIC bus frequency: ~%lu Hz\n", (unsigned long)(ticks * 18));
    return ticks * 18;  /* PIT period is ~1/18.2 second */
}

/* ── I/O APIC ─────────────────────────────────────────────────────── */

static volatile uint32_t *ioapic = NULL;

/* Maximum I/O APIC redirection entry index (set during ioapic_init) */
static uint8_t ioapic_nr_entries = 0;

static void ioapic_write_reg(uint32_t reg, uint32_t val) {
    ioapic[IOAPIC_INDEX / 4] = reg;
    ioapic[IOAPIC_DATA / 4] = val;
}

static uint32_t ioapic_read_reg(uint32_t reg) {
    ioapic[IOAPIC_INDEX / 4] = reg;
    return ioapic[IOAPIC_DATA / 4];
}

void ioapic_init(void) {
    /* Map I/O APIC at fixed virtual address */
    vmm_map_page(IOAPIC_BASE_VIRT, 0xFEC00000ULL,
                        VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);
    ioapic = (volatile uint32_t *)IOAPIC_BASE_VIRT;

    uint32_t id = ioapic_read_reg(IOAPIC_ID);
    uint32_t ver = ioapic_read_reg(IOAPIC_VER);
    uint8_t max_redir = (ver >> 16) & 0xFF;
    ioapic_nr_entries = max_redir;  /* store for bounds checks in other routines */

    kprintf("[OK] I/O APIC (ID=0x%lx, max_redir=%u)\n", (unsigned long)(id >> 24), (unsigned int)(max_redir + 1));

    /* Mask all redirection entries by setting the mask bit */
    for (int i = 0; i <= max_redir; i++) {
        ioapic_write_reg(IOAPIC_REDTBL + i * 2, IOAPIC_MASKED);
        ioapic_write_reg(IOAPIC_REDTBL + i * 2 + 1, 0);
    }

    /* Set first 16 pins to ExtINT delivery (legacy PIC-compatible mode).
     * On many chipsets the PIT, keyboard, RTC, etc. are only connected
     * to the PIC, not directly to the I/O APIC.  ExtINT forwards the
     * PIC's output through the I/O APIC, so each driver only needs
     * pic_unmask() + ioapic_unmask_irq(). */
    for (int i = 0; i < 16 && i <= max_redir; i++) {
        uint32_t low = IOAPIC_MASKED | (7 << 8);
        ioapic_write_reg(IOAPIC_REDTBL + i * 2, low);
        ioapic_write_reg(IOAPIC_REDTBL + i * 2 + 1, 0);
    }
}

void ioapic_redirect_extint(uint8_t irq) {
    /* Bounds check: ensure irq does not exceed I/O APIC redirection entries */
    if (irq > ioapic_nr_entries)
        return;
    /* Configure I/O APIC pin for ExtINT delivery.
     * ExtINT forwards whatever the legacy PIC outputs — used for
     * backward-compatible interrupt routing (PIT, keyboard, etc.). */
    uint32_t low = (7 << 8); /* ExtINT, phys, edge */
    ioapic_write_reg(IOAPIC_REDTBL + irq * 2, low);
    ioapic_write_reg(IOAPIC_REDTBL + irq * 2 + 1, 0);
}

/* Universal interrupt acknowledge — sends EOI to both APIC and PIC.
 * Under ExtINT routing both controllers must be acknowledged. */
void irq_ack(uint8_t irq) {
    if (apic_is_init_complete())
        apic_eoi();
    pic_eoi(irq);
}

void ioapic_redirect_irq(uint8_t irq, uint8_t vector, uint32_t apic_id) {
    /* Bounds check: ensure irq does not exceed I/O APIC redirection entries */
    if (irq > ioapic_nr_entries)
        return;
    uint32_t low = vector | (apic_id << 24);  /* physical destination mode */
    uint32_t high = 0;
    ioapic_write_reg(IOAPIC_REDTBL + irq * 2, low);
    ioapic_write_reg(IOAPIC_REDTBL + irq * 2 + 1, high);
}

void ioapic_mask_irq(uint8_t irq) {
    /* Bounds check: ensure irq does not exceed I/O APIC redirection entries */
    if (irq > ioapic_nr_entries)
        return;
    uint32_t low = ioapic_read_reg(IOAPIC_REDTBL + irq * 2);
    ioapic_write_reg(IOAPIC_REDTBL + irq * 2, low | IOAPIC_MASKED);
}

void ioapic_unmask_irq(uint8_t irq) {
    /* Bounds check: ensure irq does not exceed I/O APIC redirection entries */
    if (irq > ioapic_nr_entries)
        return;
    uint32_t low = ioapic_read_reg(IOAPIC_REDTBL + irq * 2);
    ioapic_write_reg(IOAPIC_REDTBL + irq * 2, low & ~IOAPIC_MASKED);
}

/* ── IPI (Inter-Processor Interrupts) handlers ────────────────────── */

#define TLB_SHOOTDOWN_MAX 64
struct tlb_shootdown_info {
    volatile int active_request;
    volatile int nr_addrs;
    uint64_t addrs[TLB_SHOOTDOWN_MAX];
    volatile int finished;
};

static struct tlb_shootdown_info tlb_info[SMP_MAX_CPUS];

/* Backtrace IPI: dump register state, kallsyms symbol, process info,
 * and stack backtrace on the receiving CPU.
 * Called when another CPU detects a lockup and wants diagnostic info. */
void ipi_backtrace_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint32_t cpu_id = smp_get_cpu_id();
    kprintf("\n=== BACKTRACE REQUEST on CPU %u ===\n", (unsigned int)cpu_id);
    dump_regs();

    /* Show symbol at the interrupted RIP */
    struct process *cur = get_current_process();
    if (cur) {
        kprintf("Process: %s (pid=%u, state=%d, is_user=%d)\n",
                cur->name ? cur->name : "?",
                cur->pid, (int)cur->state, cur->is_user);
        kprintf("Kernel stack: 0x%llx - 0x%llx\n",
                (unsigned long long)cur->kernel_stack,
                (unsigned long long)cur->stack_top);
    }

    /* Dump control registers for full diagnostic */
    {
        uint64_t cr0, cr2, cr3, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        kprintf("CR0: 0x%lx  CR2: 0x%lx  CR3: 0x%lx  CR4: 0x%lx\n",
                (unsigned long)cr0, (unsigned long)cr2,
                (unsigned long)cr3, (unsigned long)cr4);
    }

    dump_stack();
    kprintf("=== END BACKTRACE CPU %u ===\n", (unsigned int)cpu_id);
    apic_eoi();
}

/* Register IPI handlers in the IDT */
void ipi_init(void) {
    idt_register_handler(IPI_VECTOR_RESCHEDULE, ipi_reschedule_handler);
    idt_register_handler(IPI_VECTOR_TLB_SHOOT,   ipi_tlb_shootdown_handler);
    idt_register_handler(IPI_VECTOR_BACKTRACE,   ipi_backtrace_handler);
    idt_register_handler(IPI_VECTOR_MEMBARRIER,  ipi_membarrier_handler);
    idt_register_handler(IPI_VECTOR_PANIC_HALT,  ipi_panic_halt_handler);
    memset(tlb_info, 0, sizeof(tlb_info));

    /* Initialize stop_machine subsystem (registers its own IPI handler) */
    stop_machine_init();
}

/* Reschedule IPI: triggers the receiving CPU to re-evaluate the scheduler.
 * The next timer tick or interrupt will cause a reschedule anyway, so this
 * is mainly a kick to get out of HLT. */
void ipi_reschedule_handler(struct interrupt_frame *frame) {
    (void)frame;
    apic_eoi();
}

/* TLB shootdown IPI: invalidate all pending addresses on this CPU */
void ipi_tlb_shootdown_handler(struct interrupt_frame *frame) {
    (void)frame;
    int cpu_id = smp_get_cpu_id();

    struct tlb_shootdown_info *info = &tlb_info[cpu_id];
    int nr = info->nr_addrs;
    for (int i = 0; i < nr; i++) {
        __asm__ volatile("invlpg (%0)" : : "r"(info->addrs[i]) : "memory");
    }
    info->finished = 1;
    apic_eoi();
}

/* Memory barrier IPI: execute a full memory barrier and return.
 * Used by the membarrier syscall to force visibility of memory
 * operations across all CPUs. */
void ipi_membarrier_handler(struct interrupt_frame *frame) {
    (void)frame;
    /* Full memory barrier — all previous loads/stores complete
     * before any subsequent loads/stores are visible. */
    __asm__ volatile("mfence" ::: "memory");
    apic_eoi();
}

/* Panic halt IPI handler: called on all non-panicking CPUs when
 * one CPU panics.  Disables interrupts and halts forever to prevent
 * other CPUs from continuing execution after a kernel panic. */
void ipi_panic_halt_handler(struct interrupt_frame *frame) {
    (void)frame;
    /* Disable interrupts and halt this CPU forever.  We do NOT
     * acknowledge the IPI (no apic_eoi) because the panicking CPU
     * is already in an unrecoverable state — we want this CPU to
     * stop immediately and not process any further interrupts. */
    cli();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* Send TLB shootdown to all other CPUs.
 * Queues addresses, sends IPI, spins until completion.
 *
 * Optimization: if only one CPU is online, or if the current process is
 * pinned to a single CPU (via cpu_affinity), no other CPU can have stale
 * TLB entries for this address space — skip the IPI entirely and just
 * invalidate locally. */
void smp_tlb_shootdown(const uint64_t *addrs, int nr) {
    int cpu_count = smp_get_cpu_count();

    if (cpu_count <= 1) {
        /* Single CPU: local invalidation only */
        for (int i = 0; i < nr; i++)
            __asm__ volatile("invlpg (%0)" : : "r"(addrs[i]) : "memory");
        return;
    }

    /* ── Fast-path: process pinned to this single CPU (Item 286) ───── */
    struct process *current = get_current_process();
    if (current) {
        uint8_t affinity = current->cpu_affinity;
        /* cpu_affinity == 0 means "all CPUs".  Non-zero with exactly one
         * bit set means the process is pinned to that single CPU. */
        if (affinity != 0 && (affinity & (affinity - 1)) == 0) {
            int my_cpu = smp_get_cpu_id();
            if ((affinity & (1U << my_cpu)) != 0) {
                /* Process is pinned to this CPU only — no other CPU can
                 * have this address space loaded.  Local invalidation
                 * suffices. */
                for (int i = 0; i < nr; i++)
                    __asm__ volatile("invlpg (%0)" : : "r"(addrs[i]) : "memory");
                return;
            }
        }
    }

    /* ── Fast-path: only one CPU online (hotplug scenario) ─────────── */
    if (cpuhp_online_count() <= 1) {
        for (int i = 0; i < nr; i++)
            __asm__ volatile("invlpg (%0)" : : "r"(addrs[i]) : "memory");
        return;
    }

    int my_cpu = smp_get_cpu_id();

    /* Write addresses to each remote CPU's info struct */
    for (int cpu = 0; cpu < cpu_count; cpu++) {
        if (cpu == my_cpu) continue;
        struct tlb_shootdown_info *info = &tlb_info[cpu];
        while (info->active_request) __asm__ volatile("pause");
        info->active_request = 1;
        info->nr_addrs = (nr < TLB_SHOOTDOWN_MAX) ? nr : TLB_SHOOTDOWN_MAX;
        for (int i = 0; i < info->nr_addrs; i++)
            info->addrs[i] = addrs[i];
        info->finished = 0;
    }

    /* Send IPI to all other CPUs */
    __asm__ volatile("mfence" ::: "memory");
    apic_send_ipi_all_except(IPI_VECTOR_TLB_SHOOT);

    /* Also invalidate local TLB (our own page tables changed too) */
    for (int i = 0; i < nr; i++)
        __asm__ volatile("invlpg (%0)" : : "r"(addrs[i]) : "memory");

    /* Wait for remote ACKs */
    for (int cpu = 0; cpu < cpu_count; cpu++) {
        if (cpu == my_cpu) continue;
        while (!tlb_info[cpu].finished) __asm__ volatile("pause");
        tlb_info[cpu].active_request = 0;
    }
}

/* ── Exported symbols for driver modules ─────────────────────────── */
EXPORT_SYMBOL(ioapic_mask_irq);
EXPORT_SYMBOL(ioapic_unmask_irq);

/* ── Stub: apic_calibrate_timer ─────────────────────────────── */
static int apic_calibrate_timer(void)
{
    kprintf("[apic] apic_calibrate_timer: not yet implemented\n");
    return 0;
}
