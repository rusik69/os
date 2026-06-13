/* usercopy.c — Usercopy hardening
 *
 * Provides whitelist-based validation for copy_to_user / copy_from_user
 * operations.  Detects heap/stack buffer overflows by checking that
 * the kernel-side buffer lies within known-valid regions
 * (stack, heap, .data, .bss).
 *
 * Inspired by the Linux kernel's CONFIG_HARDENED_USERCOPY.
 */

#include "types.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "vmm.h"
#include "slab.h"
#include "panic.h"
#include "errno.h"

/* ── Configuration ─────────────────────────────────────────────────── */

static int usercopy_hardening_enabled = 1;

/* Statistics */
static uint64_t usercopy_checks_total  = 0;
static uint64_t usercopy_rejected      = 0;

/* ── Linker section boundaries ─────────────────────────────────────── */

extern char _text_start[], _text_end[];
extern char _rodata_start[], _rodata_end[];
extern char _data_start[], _data_end[];
extern char _bss_start[], _bss_end[];

/* ── Region types ──────────────────────────────────────────────────── */

#define REGION_UNKNOWN   0
#define REGION_TEXT      1
#define REGION_RODATA    2
#define REGION_DATA      3
#define REGION_BSS       4
#define REGION_STACK     5
#define REGION_HEAP      6
#define REGION_SLAB      7

/* ── Check which region a kernel address belongs to ────────────────── */

static int classify_kernel_addr(uint64_t addr, size_t len,
                                 uint64_t *region_start, uint64_t *region_end)
{
    uint64_t start = addr;
    uint64_t end   = addr + len;

    /* Check text section */
    if (start >= (uint64_t)_text_start && end <= (uint64_t)_text_end) {
        *region_start = (uint64_t)_text_start;
        *region_end   = (uint64_t)_text_end;
        return REGION_TEXT;
    }

    /* Check rodata section */
    if (start >= (uint64_t)_rodata_start && end <= (uint64_t)_rodata_end) {
        *region_start = (uint64_t)_rodata_start;
        *region_end   = (uint64_t)_rodata_end;
        return REGION_RODATA;
    }

    /* Check data section */
    if (start >= (uint64_t)_data_start && end <= (uint64_t)_data_end) {
        *region_start = (uint64_t)_data_start;
        *region_end   = (uint64_t)_data_end;
        return REGION_DATA;
    }

    /* Check bss section */
    if (start >= (uint64_t)_bss_start && end <= (uint64_t)_bss_end) {
        *region_start = (uint64_t)_bss_start;
        *region_end   = (uint64_t)_bss_end;
        return REGION_BSS;
    }

    /* Check kernel stack (per-process) */
    struct process *p = process_get_current();
    if (p && p->kernel_stack) {
        uint64_t stack_start = (uint64_t)p->kernel_stack;
        uint64_t stack_end   = stack_start + 4096; /* typical kernel stack size */
        if (start >= stack_start && end <= stack_end) {
            *region_start = stack_start;
            *region_end   = stack_end;
            return REGION_STACK;
        }
    }

    /* Check heap/slab */
    /* In a full implementation, iterate slab caches to check allocation bounds */
    if (slab_check_addr((void *)addr)) {
        *region_start = 0;
        *region_end   = 0;
        return REGION_SLAB;
    }

    /* Check heap */
    if (heap_check_addr((void *)addr)) {
        *region_start = 0;
        *region_end   = 0;
        return REGION_HEAP;
    }

    return REGION_UNKNOWN;
}

/* ── Main validation function ──────────────────────────────────────── */

int usercopy_validate(const void *kptr, size_t len, int to_user)
{
    if (!usercopy_hardening_enabled) return 0;
    if (!kptr || len == 0) return 0;

    uint64_t addr = (uint64_t)kptr;
    usercopy_checks_total++;

    uint64_t rstart = 0, rend = 0;
    int region = classify_kernel_addr(addr, len, &rstart, &rend);

    /* Reject unknown regions */
    if (region == REGION_UNKNOWN) {
        goto reject;
    }

    /* Reject text/rodata for user copies (should never write to .text) */
    if (region == REGION_TEXT && to_user == 0) {
        /* Kernel reading from user into .text is suspicious */
        goto reject;
    }

    /* Reject code sections for user copies */
    if (region == REGION_TEXT || region == REGION_RODATA) {
        goto reject;
    }

    /* Bounds check: ensure entire buffer lies within the region */
    if (rstart != 0 && rend != 0) {
        if (addr < rstart || (addr + len) > rend) {
            goto reject;
        }
    }

    return 0; /* OK */

reject:
    if (usercopy_hardening_enabled) {
        usercopy_rejected++;
        struct process *p = process_get_current();
        kprintf("[USERCOPY] REJECTED: %s kptr=0x%016llx len=%zu "
                "region=%d pid=%d\n",
                to_user ? "to_user" : "from_user",
                (unsigned long long)addr, len,
                region, p ? p->pid : -1);
        return -EFAULT;
    }
    return 0;
}

/* ── Query / status ────────────────────────────────────────────────── */

int usercopy_get_enabled(void) { return usercopy_hardening_enabled; }
void usercopy_set_enabled(int val) { usercopy_hardening_enabled = val ? 1 : 0; }
uint64_t usercopy_get_checks(void) { return usercopy_checks_total; }
uint64_t usercopy_get_rejected(void) { return usercopy_rejected; }

/* ── Initialization ────────────────────────────────────────────────── */

void usercopy_init(void)
{
    kprintf("[OK] USERCOPY: copy_to/from_user hardening active\n");
}
