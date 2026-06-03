#ifndef SUSPEND_H
#define SUSPEND_H

#include "types.h"

/*
 * Suspend-to-RAM (ACPI S3) support.
 *
 * Saves CPU and system state, enters ACPI S3 sleep state via
 * the ACPI PM1a_CNT register, and restores state on resume.
 *
 * Reference: ACPI Specification v6.5, Section 4.8.4
 */

/* ── Suspend state save area ─────────────────────────────────────────── */
/* This structure must reside in a page that survives S3 (RAM is
 * preserved in self-refresh during S3). */
struct suspend_state {
    /* GDT / IDT pseudo-descriptors */
    uint64_t gdt_base;
    uint32_t gdt_limit;
    uint64_t idt_base;
    uint32_t idt_limit;

    /* Control registers */
    uint64_t cr0, cr2, cr3, cr4;

    /* Segment selectors */
    uint16_t cs, ds, es, fs, gs, ss;

    /* Extended Feature Enable Register (EFER MSR 0xC0000080) */
    uint64_t efer;

    /* Saved stack pointer for resume */
    uint64_t resume_rsp;

    /* Resume return address (address of our restore function) */
    uint64_t resume_rip;

    /* Magic number for sanity check after resume */
    uint32_t magic;
#define SUSPEND_MAGIC 0x53555330  /* "SUS0" */
} __attribute__((packed));

/* ── Public API ──────────────────────────────────────────────────────── */

/**
 * suspend_s3() — Enter ACPI S3 Suspend-to-RAM state.
 *
 * Saves CPU/system state, calls the ACPI PM1a_CNT sleep entry,
 * and restores state on resume.
 *
 * Returns 0 on successful resume, negative on error.
 *
 * NOTE: On real hardware, if S3 succeeds the function will not return
 * until after a wake event (power button, RTC alarm, etc.).  The saved
 * CPU state is restored transparently.
 */
int suspend_s3(void);

#endif /* SUSPEND_H */
