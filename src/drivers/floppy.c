#include "floppy.h"
#include "printf.h"

/*
 * floppy.c — Floppy disk controller driver
 *
 * Supports dual build as a loadable kernel module (.ko) or compiled
 * directly into the kernel.  When MODULE is defined the module loader
 * calls init_module() / cleanup_module(); otherwise the built-in path
 * calls floppy_init() directly.
 *
 * NOTE: This driver is currently a stub — no floppy controller is
 * present in typical QEMU/VM setups.  The IRQ vector for IRQ 6 is
 * reserved in idt.c for future implementation.
 */

#ifdef MODULE
#include "module.h"
#endif

/* ── Initialisation ──────────────────────────────────────────────── */

int floppy_init(void) {
    /* Stub: no floppy controller present in typical QEMU/VM setup */
    kprintf("[--] Floppy: no controller found\n");
    return -1;
}

/* ── Module hotplug (loadable module path) ───────────────────────── */

#ifdef MODULE

/* Module entry point — called by the ELF module loader on insmod. */
int init_module(void) {
    return floppy_init();
}

/* Module exit point — called by the ELF module loader on rmmod. */
void cleanup_module(void) {
    /* Nothing to clean up (stub driver). */
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Floppy disk controller driver (stub)");

#endif /* MODULE */

/* ── Driver API ──────────────────────────────────────────────────── */

int floppy_is_present(void) {
    return 0;
}

int floppy_read_sectors(int drive, uint32_t lba, uint8_t count, void *buf) {
    (void)drive;
    (void)lba;
    (void)count;
    (void)buf;
    /* Stub: always fails */
    return -1;
}
