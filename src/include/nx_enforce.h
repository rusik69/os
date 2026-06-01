#ifndef NX_ENFORCE_H
#define NX_ENFORCE_H

#include "types.h"

/*
 * NX (No-Execute) bit enforcement.
 *
 * Provides runtime verification that:
 *   1. The NXE bit is enabled in IA32_EFER (done during boot)
 *   2. Kernel .text pages are executable (NX cleared)
 *   3. All other pages (.rodata, .data, .bss, stack, heap) have NX set
 *   4. Page-fault handler detects NX violations from both kernel and user mode
 *
 * Usage:
 *   nx_enforce_init();            // one-time at boot
 *   nx_enforce_audit_kernel();    // verify all kernel pages after init
 *   nx_enforce_register_region(); // subsystem adds executable region (e.g., module code)
 *   nx_enforce_is_executable();   // query if address is in a known exec region
 *   nx_enforce_check_fault();     // called from page-fault handler
 */

/* Maximum number of tracked executable regions */
#define NX_ENFORCE_MAX_REGIONS 32

/* ── Initialisation ──────────────────────────────────────────── */

/**
 * nx_enforce_init - Enable NX bit (EFER.NXE) and register kernel .text range.
 * Must be called once during boot, after paging is enabled.
 * Returns 0 on success, -1 if NX not supported.
 */
int nx_enforce_init(void);

/**
 * nx_enforce_is_active - Returns 1 if NX enforcement was successfully enabled.
 */
int nx_enforce_is_active(void);

/* ── Executable region tracking ──────────────────────────────── */

/**
 * nx_enforce_register_region - Add a virtual address range that is allowed
 * to be executable.  Used by the module loader, JIT compiler, etc.
 * Returns 0 on success, -1 if the region table is full.
 */
int nx_enforce_register_region(uint64_t start, uint64_t end);

/**
 * nx_enforce_is_executable - Check whether @vaddr falls inside any
 * registered executable region.
 */
int nx_enforce_is_executable(uint64_t vaddr);

/* ── Audit ───────────────────────────────────────────────────── */

/**
 * nx_enforce_audit_kernel - Walk the kernel page table and verify that:
 *   - .text pages have NX cleared (executable)
 *   - .rodata, .data, .bss, and all other pages have NX set
 * Logs all violations and returns the count.
 */
int nx_enforce_audit_kernel(void);

/**
 * nx_enforce_audit_user_range - Audit a user-space page range for
 * correct NX placement based on the protection flags.
 */
int nx_enforce_audit_user_range(uint64_t *pml4, uint64_t start, uint64_t end);

/* ── Page-fault integration ──────────────────────────────────── */

/**
 * nx_enforce_check_fault - Called from the page-fault handler when
 * the error code indicates an instruction fetch (bit 4 set) and a
 * protection violation (bit 0 set).  Returns 1 if the fault was
 * identified as an NX violation (and appropriate action was taken),
 * 0 otherwise.
 *
 * For user-mode violations: delivers SIGSEGV.
 * For kernel-mode violations: panics with a full register dump.
 *
 * @cr2:   Faulting virtual address (from CR2)
 * @err:   Page-fault error code
 * @frame: Interrupt frame (register state at the time of fault)
 */
struct interrupt_frame;
int nx_enforce_check_fault(uint64_t cr2, uint64_t err,
                           struct interrupt_frame *frame);

/* ── Reporting ───────────────────────────────────────────────── */

/**
 * nx_enforce_print_regions - Dump all registered executable regions
 * to the kernel log (for /proc or debug).
 */
void nx_enforce_print_regions(void);

#endif /* NX_ENFORCE_H */
