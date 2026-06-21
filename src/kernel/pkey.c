/*
 * pkey.c — Memory Protection Keys for Userspace (pkey)
 *
 * Provides access to x86 PKU (Protection Key Unit) for userspace memory
 * protection key management. Falls back gracefully when PKU is not
 * available (returns -1 from all operations).
 *
 * PKU allows tagging pages with a 4-bit protection key and setting
 * per-key access rights (read/write/disable) via the PKRU register.
 * IA32_PKRS (MSR 0x6E0) is the supervisor-mode equivalent used by
 * the kernel for its own mappings.
 */

#include "pkey.h"
#include "printf.h"
#include "string.h"
#include "cpu.h"
#include "vmm.h"
#include "process.h"
#include "spinlock.h"

/* ── PKU availability ──────────────────────────────────────────────── */

/* Cached PKU availability flag */
static int g_pku_available = -1;  /* -1 = not yet probed */

/* IA32_PKRS MSR (for supervisor-mode protection keys) */
#define IA32_PKRS 0x6E0ULL

/* Protection key bits in x86-64 PTEs: bits 59:62 encode the 4-bit key */
#define PTE_PKEY_MASK  0x3C0000000000000ULL  /* bits 59-62 */
#define PTE_PKEY_SHIFT 59

/* Bitmap of allocated keys: bit N set = key N allocated */
static uint16_t g_pkey_allocated = 0;

/* Protects g_pkey_allocated — since this is simple bitmap, we just use
 * simple non-reentrant update.  In practice, pkey_alloc/free are called
 * during process setup, not from interrupt handlers. */
static int g_pkey_lock = 0;

/* ── Internal helpers ──────────────────────────────────────────────── */

static int pkey_probe_pku(void)
{
    if (g_pku_available != -1)
        return g_pku_available;

    /* Check CPUID leaf 07h, ECX bit 3 (OSXSAVE), bit 4 (PKU) */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x07), "c"(0x0));
    int pku = (ecx >> 4) & 1;   /* bit 4: PKU */
    int ospke = (ecx >> 2) & 1; /* bit 2: OS has enabled XSAVE + PKRU */

    g_pku_available = (pku && ospke) ? 1 : 0;

    if (g_pku_available) {
        kprintf("[pkey] PKU available (%d-bit keys)\n",
                (int)sizeof(uint16_t) * 8);
    } else {
        kprintf("[pkey] PKU not available\n");
    }
    return g_pku_available;
}

static void pkey_write_pkru(uint32_t pkru)
{
    /* PKRU is the XSAVE-managed register (XRSTOR manages it).
     * On CPUs with PKU, we write it via PKRU instruction.
     * The kernel should never use RDPKRU/WRPKRU in kernel mode
     * (they're user instructions). Instead, use IA32_PKRS MSR
     * for kernel access.
     *
     * For userspace protection keys, the kernel traps the pkey
     * syscalls and the actual PKRU is managed per-process via XSAVE.
     * Our pkey implementation is a stub — on a real system, pkey_alloc
     * would assign a key, pkey_mprotect would set the PTE protection
     * key bits, and the PKRU value would be context-switched via XSAVE.
     *
     * Here, for the supervisor-mode interface:
     */
    write_msr(IA32_PKRS, (uint64_t)pkru);
}

static uint32_t pkey_read_pkru(void)
{
    return (uint32_t)read_msr(IA32_PKRS);
}

/* ── Public API ─────────────────────────────────────────────────────── */

int pkey_pku_available(void)
{
    return pkey_probe_pku();
}

int pkey_alloc(unsigned int flags, unsigned int rights)
{
    (void)flags;  /* Unused for now */

    if (!pkey_probe_pku())
        return -1;

    /* Find the first free key (skip key 0, which is the default) */
    int pkey = -1;
    for (int i = 1; i < PKEY_MAX; i++) {
        if (!(g_pkey_allocated & (1U << i))) {
            pkey = i;
            break;
        }
    }

    if (pkey < 0) {
        kprintf("[pkey] No keys available\n");
        return -1;
    }

    /* Mark as allocated */
    g_pkey_allocated |= (uint16_t)(1U << pkey);

    /* Set initial rights */
    if (rights != 0) {
        pkey_set_rights(pkey, rights);
    }

    kprintf("[pkey] Allocated key %d with rights 0x%x\n", pkey, rights);
    return pkey;
}

int pkey_free(int pkey)
{
    if (pkey < 0 || pkey >= PKEY_MAX) {
        kprintf("[pkey] Invalid key %d\n", pkey);
        return -1;
    }

    if (!(g_pkey_allocated & (1U << pkey))) {
        kprintf("[pkey] Key %d not allocated\n", pkey);
        return -1;
    }

    /* Clear allocated bit */
    g_pkey_allocated &= (uint16_t)~(1U << pkey);

    /* Reset rights to full access */
    pkey_set_rights(pkey, 0);

    return 0;
}

int pkey_mprotect(void *addr, size_t len, int prot, int pkey)
{
    (void)prot;

    if (!pkey_probe_pku())
        return -1;

    if (pkey < -1 || pkey >= PKEY_MAX)
        return -1;

    if (pkey >= 0 && !(g_pkey_allocated & (1U << pkey))) {
        kprintf("[pkey] Key %d not allocated (pkey_mprotect)\n", pkey);
        return -1;
    }

    /* Validate alignment */
    uint64_t vaddr = (uint64_t)addr;
    if (vaddr & 0xFFFULL) return -1;      /* addr not page-aligned */
    if (len == 0) return 0;

    /* Get current process page table */
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4) return -1;
    uint64_t *pml4 = proc->pml4;

    size_t num_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t vaddr_end = vaddr + num_pages * PAGE_SIZE;
    if (vaddr_end < vaddr || vaddr_end > USER_VADDR_MAX) return -1;

    /* Walk the page table and set protection key bits */
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t curr = vaddr + i * PAGE_SIZE;
        int pml4_idx = (int)((curr >> 39) & 0x1FF);
        int pdpt_idx = (int)((curr >> 30) & 0x1FF);
        int pd_idx   = (int)((curr >> 21) & 0x1FF);
        int pt_idx   = (int)((curr >> 12) & 0x1FF);

        if (!(pml4[pml4_idx] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) continue;

        /* Handle 2MB huge pages */
        if (pd[pd_idx] & (1ULL << 7)) {
            uint64_t pde = pd[pd_idx];
            uint64_t pkey_bits = (uint64_t)(pkey >= 0 ? pkey : 0) << PTE_PKEY_SHIFT;
            pd[pd_idx] = (pde & ~PTE_PKEY_MASK) | (pkey_bits & PTE_PKEY_MASK);
            /* Flush TLB for the huge page region */
            /* tlb_flush(curr & ~(HUGE_PAGE_SIZE - 1ULL)); */
            /* Skip remaining pages covered by this huge page */
            uint64_t remaining = HUGE_PAGE_SIZE / PAGE_SIZE -
                (i % (HUGE_PAGE_SIZE / PAGE_SIZE));
            if (remaining > 1) {
                i += remaining - 1;
            }
            continue;
        }

        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        if (!(pt[pt_idx] & PTE_PRESENT)) continue;

        uint64_t pte = pt[pt_idx];
        uint64_t pkey_bits = (uint64_t)(pkey >= 0 ? pkey : 0) << PTE_PKEY_SHIFT;
        pt[pt_idx] = (pte & ~PTE_PKEY_MASK) | (pkey_bits & PTE_PKEY_MASK);
        /* tlb_flush(curr); */
    }

    kprintf("[pkey] mprotect addr=%p len=%llu prot=%d pkey=%d\n",
            addr, (unsigned long long)len, prot, pkey);
    return 0;
}

int pkey_set_rights(int pkey, unsigned int rights)
{
    if (!pkey_probe_pku())
        return -1;

    if (pkey < 0 || pkey >= PKEY_MAX)
        return -1;

    if (rights & ~(PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE))
        return -1;

    /* Each key uses 2 bits in PKRU at bit position pkey*2.
     *   bit[pkey*2]   = 1 -> disable access
     *   bit[pkey*2+1] = 1 -> disable write */
    uint32_t pkru = pkey_read_pkru();
    int shift = pkey * 2;
    pkru &= ~(3U << shift);       /* Clear both bits */
    pkru |= (rights & 3) << shift; /* Set new rights */
    pkey_write_pkru(pkru);

    return 0;
}

int pkey_get_rights(int pkey)
{
    if (!pkey_probe_pku())
        return -1;

    if (pkey < 0 || pkey >= PKEY_MAX)
        return -1;

    uint32_t pkru = pkey_read_pkru();
    return (int)((pkru >> (pkey * 2)) & 3);
}

/* ── Stub: sys_pkey_alloc ─────────────────────────────────────────────── */
int sys_pkey_alloc(unsigned int flags, unsigned int rights)
{
    (void)flags;
    (void)rights;
    kprintf("[pkey] sys_pkey_alloc not yet fully implemented\n");
    return pkey_alloc(flags, rights);
}

/* ── Stub: sys_pkey_free ──────────────────────────────────────────────── */
int sys_pkey_free(int pkey)
{
    (void)pkey;
    kprintf("[pkey] sys_pkey_free not yet fully implemented\n");
    return pkey_free(pkey);
}

/* ── Stub: sys_pkey_mprotect ──────────────────────────────────────────── */
int sys_pkey_mprotect(void *addr, size_t len, int prot, int pkey)
{
    (void)addr;
    (void)len;
    (void)prot;
    (void)pkey;
    kprintf("[pkey] sys_pkey_mprotect not yet implemented\n");
    return -ENOSYS;
}
