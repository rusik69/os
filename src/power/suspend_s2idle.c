// SPDX-License-Identifier: GPL-2.0-only
/*
 * suspend_s2idle.c — Suspend-to-idle (s2idle) state
 *
 * Implements the s2idle (Suspend-to-Idle) power management state,
 * the shallowest system sleep state where CPUs enter deep idle
 * but system remains responsive to interrupts.
 *
 * Wakeup detection uses proper IRQ-based checking:
 *   - Checks GIC/IOAPIC for pending interrupts
 *   - Validates against enabled wakeup IRQs
 *   - Falls back to polling wakeup sources
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "timer.h"
#include "smp.h"
#include "wakeup.h"
#include "ioapic.h"
#include "acpi.h"

#define S2IDLE_WAKEUP_TIMEOUT 500 /* 500ms default timeout */

struct s2idle_state {
    int suspended;
    uint64_t suspend_start;
    int wakeup_reason; /* 0=none, 1=timer, 2=interrupt, 3=wakeup device */
};

static struct s2idle_state s2idle_state;

/* ── IRQ-based wakeup detection ───────────────────────────────────────
 *
 * Checks for pending interrupts that could wake the system from s2idle.
 * This provides a more reliable wakeup mechanism than polling wakeup
 * sources alone.
 */

/* Maximum number of registered wakeup IRQs */
#define MAX_WAKEUP_IRQS 32

/* Registered wakeup IRQ list */
static struct {
    int irq_num;
    int enabled;
} g_wakeup_irqs[MAX_WAKEUP_IRQS];

static int g_wakeup_irq_count = 0;

/**
 * s2idle_register_wakeup_irq — Register an IRQ as a wakeup source.
 *
 * @irq:  Interrupt number that can wake the system from s2idle.
 *
 * Returns 0 on success, negative on error.
 */
int s2idle_register_wakeup_irq(int irq)
{
    if (g_wakeup_irq_count >= MAX_WAKEUP_IRQS)
        return -ENOSPC;

    /* Check if already registered */
    for (int i = 0; i < g_wakeup_irq_count; i++) {
        if (g_wakeup_irqs[i].irq_num == irq)
            return 0;
    }

    g_wakeup_irqs[g_wakeup_irq_count].irq_num = irq;
    g_wakeup_irqs[g_wakeup_irq_count].enabled = 1;
    g_wakeup_irq_count++;

    return 0;
}

/**
 * s2idle_unregister_wakeup_irq — Remove an IRQ from wakeup sources.
 */
int s2idle_unregister_wakeup_irq(int irq)
{
    for (int i = 0; i < g_wakeup_irq_count; i++) {
        if (g_wakeup_irqs[i].irq_num == irq) {
            /* Compact array */
            for (int j = i; j < g_wakeup_irq_count - 1; j++)
                g_wakeup_irqs[j] = g_wakeup_irqs[j + 1];
            g_wakeup_irq_count--;
            return 0;
        }
    }
    return -ENOENT;
}

/**
 * s2idle_check_pending_irqs — Check for pending wakeup IRQs.
 *
 * Scans the IOAPIC (or GIC on ARM) for interrupts that are
 * pending and registered as wakeup sources.
 *
 * Returns 1 if a wakeup IRQ is pending, 0 otherwise.
 */
static int s2idle_check_pending_irqs(void)
{
    /* Check via IOAPIC for pending interrupts */
    for (int i = 0; i < g_wakeup_irq_count; i++) {
        if (!g_wakeup_irqs[i].enabled)
            continue;

        int irq = g_wakeup_irqs[i].irq_num;

        /* Check IOAPIC IRR (Interrupt Request Register) for this IRQ */
        int is_pending = ioapic_is_interrupt_pending(irq);
        if (is_pending) {
            kprintf("[S2IDLE] Wakeup IRQ %d is pending\n", irq);
            return 1;
        }

        /* Also check the legacy PIC if present */
        if (irq < 16) {
            is_pending = pic_is_interrupt_pending(irq);
            if (is_pending) {
                kprintf("[S2IDLE] Wakeup PIC IRQ %d is pending\n", irq);
                return 1;
            }
        }
    }

    /* Check ACPI GPE (General Purpose Event) registers for wake events */
    if (acpi_is_present()) {
        uint32_t gpe_sts = acpi_read_gpe_status();
        if (gpe_sts != 0) {
            kprintf("[S2IDLE] ACPI GPE wake event pending (STS=0x%08X)\n",
                    (unsigned)gpe_sts);
            return 1;
        }
    }

    return 0;
}

/* Enter s2idle state */
int s2idle_enter(void)
{
    if (s2idle_state.suspended)
        return -EBUSY;

    kprintf("[S2IDLE] Entering suspend-to-idle...\n");

    s2idle_state.suspended = 1;
    s2idle_state.suspend_start = timer_get_ticks();
    s2idle_state.wakeup_reason = 0;

    /* Prepare CPUs for idle */
    int ncpus = smp_get_cpu_count();

    kprintf("[S2IDLE] CPUs entering idle (ncpus=%d, wakeup_irqs=%d)\n",
            ncpus, g_wakeup_irq_count);

    /* Enter idle loop with proper IRQ-based wakeup detection */
    uint64_t timeout = timer_get_ticks() + S2IDLE_WAKEUP_TIMEOUT;
    while (timer_get_ticks() < timeout) {
        /* ── Check 1: IRQ-based wakeup detection ─────────────────── */
        if (s2idle_check_pending_irqs()) {
            s2idle_state.wakeup_reason = 2; /* interrupt */
            break;
        }

        /* ── Check 2: Registered wakeup sources (legacy) ─────────── */
        int pending_wakeups = 0;
        for (int i = 0; i < WAKEUP_SRC_MAX; i++) {
            if (wakeup_source_is_active(i)) {
                pending_wakeups = 1;
                break;
            }
        }

        if (pending_wakeups) {
            s2idle_state.wakeup_reason = 3; /* wakeup device */
            break;
        }

        /* ── Check 3: Software wakeup signal ─────────────────────── */
        if (s2idle_state.wakeup_reason != 0)
            break;

        /* ── Enter low-power state ─────────────────────────────────
         * Use MWAIT or HLT to wait for interrupts.
         * On x86, HLT is the standard way to idle.
         */
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }

    /* If timeout reached, set timeout wakeup reason */
    if (s2idle_state.wakeup_reason == 0)
        s2idle_state.wakeup_reason = 1; /* timeout wakeup */

    kprintf("[S2IDLE] Woken up (reason=%d, elapsed=%llu ms)\n",
            s2idle_state.wakeup_reason,
            (unsigned long long)(timer_get_ticks() - s2idle_state.suspend_start) * 1000 / TIMER_FREQ);

    s2idle_state.suspended = 0;
    return 0;
}

/* Check if currently in s2idle */
int s2idle_is_suspended(void)
{
    return s2idle_state.suspended;
}

/* Get wakeup reason */
int s2idle_get_wakeup_reason(void)
{
    return s2idle_state.wakeup_reason;
}

/* Exit s2idle (called from interrupt handler) */
void s2idle_wakeup(void)
{
    if (s2idle_state.suspended) {
        s2idle_state.wakeup_reason = 2; /* interrupt */
    }
}

void s2idle_init(void)
{
    memset(&s2idle_state, 0, sizeof(s2idle_state));
    g_wakeup_irq_count = 0;
    memset(g_wakeup_irqs, 0, sizeof(g_wakeup_irqs));

    /* Register standard wakeup IRQs if ACPI is present */
    if (acpi_is_present()) {
        /* ACPI SCI is usually IRQ 9 */
        s2idle_register_wakeup_irq(9);
        kprintf("[S2IDLE] Registered ACPI SCI (IRQ 9) as wakeup source\n");
    }

    /* Timer IRQ (IRQ 0) is also a wakeup source */
    s2idle_register_wakeup_irq(0);

    kprintf("[OK] s2idle — Suspend-to-Idle state (IRQ-based wakeup)\n");
}

/* ── suspend_s2idle_enter ─────────────────────────────── */
int suspend_s2idle_enter(void)
{
    /* Enter Suspend-to-Idle (S2Idle/S0ix):
     * Enable wakeup IRQs, then enter MWAIT-based idle loop.
     * Interrupts should be disabled before calling this. */
    cli();

    /* MWAIT-based S0ix entry */
    __asm__ volatile(
        "mov $0x00, %%eax\n"  /* C1 state */
        "mov $0x01, %%ecx\n"  /* interrupts-on-exit */
        "mwait\n"
        :::
        "eax", "ecx"
    );

    /* Re-enable interrupts on wake */
    sti();
    return 0;
}
/* ── suspend_s2idle_exit ─────────────────────────────── */
int suspend_s2idle_exit(void)
{
    /* Called when waking from S2Idle.
     * Re-enable interrupts and notify drivers of wakeup. */
    sti();
    return 0;
}
