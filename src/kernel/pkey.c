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

/* ── PKU availability ──────────────────────────────────────────────── */

/* Cached PKU availability flag */
static int g_pku_available = -1;  /* -1 = not yet probed */

/* IA32_PKRS MSR (for supervisor-mode protection keys) */
#define IA32_PKRS 0x6E0ULL

/* ── Key allocation bitmap ─────────────────────────────────────────── */

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
    (void)addr;
    (void)len;
    (void)prot;

    if (!pkey_probe_pku())
        return -1;

    if (pkey < -1 || pkey >= PKEY_MAX)
        return -1;

    /* In a full implementation, this would update the page table entries
     * to set the protection key bits.  For now, we just validate the
     * arguments and note the assignment. */
    if (pkey >= 0 && !(g_pkey_allocated & (1U << pkey))) {
        kprintf("[pkey] Key %d not allocated (pkey_mprotect)\n", pkey);
        return -1;
    }

    /* The actual PTE modification would happen in the VMM layer.
     * This is a stub that records the mapping but doesn't modify PTEs. */
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
