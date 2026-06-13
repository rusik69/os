/* cet.c — Intel CET (Control-flow Enforcement Technology) Shadow Stack (B6)
 *
 * Implements per-task shadow stacks for ROP (Return-Oriented Programming)
 * protection compatible with Intel CET specification.
 *
 * When CET is enabled:
 *   - CALL pushes return address to both the normal stack AND the shadow stack
 *   - RET pops from both stacks and compares — mismatch = #CP fault
 *   - ROP gadgets (ret without call) cause shadow stack underflow
 *
 * CPUID check: leaf 7, subleaf 0, ECX bit 7
 */

#include "cet.h"
#include "cpu.h"
#include "printf.h"
#include "string.h"
#include "pmm.h"
#include "errno.h"

/* ── Static state ────────────────────────────────────────────────── */

/* Global CET support flag (initialized once during cet_init) */
static int g_cet_supported = 0;
static int g_cet_initialized = 0;

/* ── Internal helpers ────────────────────────────────────────────── */

/** Check CPUID for CET shadow stack support.
 *  CPUID leaf 7 (sub-leaf 0), ECX bit 7 = CET_SS.
 *  Returns 1 if available, 0 otherwise. */
static int cet_check_cpuid(void) {
    uint32_t eax = 7, ebx, ecx = 0, edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(eax), "c"(ecx));

    /* ECX bit 7 = CET_SS (Shadow Stack) */
    return (ecx >> 7) & 1;
}

/** Check CPUID for CET indirect branch tracking.
 *  CPUID leaf 7 (sub-leaf 0), ECX bit 11 = CET_IBT. */
static int cet_ibt_available(void) {
    uint32_t eax = 7, ebx, ecx = 0, edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(eax), "c"(ecx));

    return (ecx >> 11) & 1;
}

/** Allocate a shadow stack page (physically contiguous, 4KB).
 *  Returns physical frame number (PFN), 0 on failure. */
static uint64_t cet_alloc_shstk_page(uint64_t *out_phys) {
    uint64_t frame = pmm_alloc_frame();
    if (!frame) {
        kprintf("[CET] Failed to allocate shadow stack page\n");
        return 0;
    }

    uint64_t phys = frame * 4096;
    void *virt = PHYS_TO_VIRT((void *)(uintptr_t)phys);
    memset(virt, 0, 4096);

    *out_phys = phys;
    return frame;
}

/** Place a shadow stack token at the base of the shadow stack.
 *  The token is an 8-byte value at the bottom (lowest address) of the
 *  shadow stack page:
 *    Bits [63:3] = address of shadow stack page (matching bits)
 *    Bit  [1]    = 1 (busy) / 0 (available)
 *    Bit  [0]    = 1 (valid token)
 */
static void cet_write_token(void *base, uint64_t phys_addr, int busy) {
    uint64_t *token = (uint64_t *)base;
    uint64_t val = phys_addr & CET_SS_TOKEN_MASK;

    val |= CET_SS_TOKEN_VALID;
    if (busy)
        val |= CET_SS_TOKEN_BUSY;

    *token = val;
}

/* ── Public API ──────────────────────────────────────────────────── */

int cet_init(void) {
    if (g_cet_initialized)
        return 0;

    /* Check CPU support */
    g_cet_supported = cet_check_cpuid();

    if (!g_cet_supported) {
        kprintf("[CET] Shadow stack NOT supported by CPU (CPUID.7.0.ECX bit 7=0)\n");
        kprintf("[CET] ROP protection cannot be enabled on this hardware\n");
        g_cet_initialized = 1;
        return -EOPNOTSUPP;
    }

    kprintf("[CET] Shadow stack supported (CPUID.7.0.ECX bit 7=1)\n");

    if (cet_ibt_available())
        kprintf("[CET] Indirect branch tracking (IBT) also available\n");

    g_cet_initialized = 1;
    return 0;
}

int cet_enable_per_task(struct cet_shadow_stack *sstk) {
    if (!sstk)
        return -EINVAL;

    if (!g_cet_supported)
        return -EOPNOTSUPP;

    /* Allocate shadow stack page */
    uint64_t phys = 0;
    uint64_t frame = cet_alloc_shstk_page(&phys);
    if (!frame)
        return -ENOMEM;

    void *virt = PHYS_TO_VIRT((void *)(uintptr_t)phys);

    /* Initialize shadow stack structure */
    sstk->base = (uint64_t)virt;
    sstk->phys_base = phys;
    sstk->size = CET_SHSTK_SIZE;
    sstk->enabled = 1;

    /* Place busy token at the base (lowest address) of the shadow stack.
     * The SSP register points just PAST the token on the shadow stack,
     * i.e., SSP = base + 8 (8 bytes for the token). */
    cet_write_token(virt, phys, 1);  /* busy token */

    /* Initial SSP (Shadow Stack Pointer) = base + 8 (past the token).
     * The shadow stack grows downward (towards lower addresses like
     * the regular stack), but the SSP initially points to just after
     * the token at the bottom. */
    sstk->ssp = (uint64_t)virt + 8;
    sstk->token_offset = 0;

    /* Configure IA32_U_CET MSR for this task:
     *   - Enable shadow stack (SHSTK_EN bit 0)
     *   - Enable ENDBRANCH tracking (ENDBR_EN bit 2)
     */
    uint64_t ucet_val = CET_SHSTK_EN | CET_ENDBR_EN;
    write_msr(MSR_IA32_U_CET, ucet_val);

    /* Set PL3 shadow stack pointer */
    write_msr(MSR_IA32_PL3_SSP, sstk->ssp);

    kprintf("[CET] Shadow stack enabled for task: base=0x%llX ssp=0x%llX phys=0x%llX\n",
            (unsigned long long)sstk->base,
            (unsigned long long)sstk->ssp,
            (unsigned long long)sstk->phys_base);

    return 0;
}

void cet_disable(struct cet_shadow_stack *sstk) {
    if (!sstk || !sstk->enabled)
        return;

    /* Clear IA32_U_CET shadow stack enable */
    write_msr(MSR_IA32_U_CET, 0);

    /* Clear SSP */
    write_msr(MSR_IA32_PL3_SSP, 0);

    /* Free the shadow stack page */
    if (sstk->phys_base) {
        uint64_t frame = sstk->phys_base / 4096;
        pmm_free_frame(frame);
    }

    kprintf("[CET] Shadow stack disabled for task\n");

    memset(sstk, 0, sizeof(*sstk));
}

int cet_is_supported(void) {
    return g_cet_supported;
}

void cet_switch_task(struct cet_shadow_stack *sstk) {
    if (!sstk || !sstk->enabled || !g_cet_supported)
        return;

    /* Restore PL3 SSP for the new task */
    write_msr(MSR_IA32_PL3_SSP, sstk->ssp);

    /* Re-enable U_CET (it may be cleared by the old task's exit) */
    uint64_t ucet_val = CET_SHSTK_EN | CET_ENDBR_EN;
    write_msr(MSR_IA32_U_CET, ucet_val);
}
