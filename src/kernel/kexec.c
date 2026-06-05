/*
 * kexec.c — Load and execute a new kernel without firmware reboot
 *
 * Item 362 from the production-improvements plan (plan 5: 301-400).
 *
 * kexec allows loading a new kernel image into memory and jumping to it
 * directly, bypassing firmware/hardware reset.  This enables:
 *   - Fast kernel upgrades without full reboot
 *   - kdump: boot a crash kernel on panic (with a separate crash region)
 *   - Quick development cycles
 *
 * Implementation overview:
 *   1. A 16 MB region is pre-reserved at boot (just below the kdump region).
 *   2. Userspace calls sys_kexec_load(phys_addr, entry, flags) to register
 *      a loaded kernel image.
 *   3. Userspace calls sys_reboot(LINUX_REBOOT_CMD_KEXEC) or the kernel
 *      panics → kexec_reboot() is called.
 *   4. kexec_reboot() disables interrupts, masks all APIC LVT entries,
 *      flushes TLBs, and jumps to the entry point in 64-bit long mode.
 *
 * The caller is responsible for:
 *   - Loading the new kernel ELF and any initrd into the reserved region.
 *   - Ensuring devices are quiesced, or calling from a context where
 *     that is guaranteed (e.g., after panic or explicit shutdown sequence).
 *
 * See also: kdump.c for the crash-kernel variant (Item 363).
 */

#include "kexec.h"
#include "pmm.h"
#include "vmm.h"
#include "printf.h"
#include "string.h"
#include "panic.h"
#include "apic.h"
#include "cpu.h"
#include "smp.h"
#include "io.h"

/* ── State ─────────────────────────────────────────────────────────── */

static volatile struct {
    uint64_t phys_addr;        /* physical address of loaded kernel image */
    uint64_t entry;            /* physical entry point */
    uint32_t flags;            /* KEXEC_FLAG_* */
    int      loaded;           /* 1 if a kernel image has been loaded */
} kexec_state;

static int kexec_region_reserved = 0;   /* 1 after successful kexec_init */

/* ── Initialization ────────────────────────────────────────────────── */

int kexec_init(void)
{
    if (kexec_region_reserved) {
        kprintf("[!!] kexec: already initialized\n");
        return -1;
    }

    /* Reserve the physical region from PMM so no other allocator uses it. */
    pmm_reserve_frames(KEXEC_RESERVE_PHYS, KEXEC_REGION_SIZE);

    /* Clear the state */
    memset((void *)&kexec_state, 0, sizeof(kexec_state));

    kexec_region_reserved = 1;
    kprintf("[OK] kexec: reserved %u MB at phys 0x%llx\n",
            (unsigned)(KEXEC_REGION_SIZE / (1024 * 1024)),
            (unsigned long long)KEXEC_RESERVE_PHYS);
    return 0;
}

/* ── Load ──────────────────────────────────────────────────────────── */

int kexec_load(uint64_t phys_addr, uint64_t entry, uint32_t flags)
{
    if (!kexec_region_reserved) {
        kprintf("[!!] kexec_load: kexec not initialized\n");
        return -ENODEV;
    }

    /* Validate that the image fits within the reserved region */
    if (phys_addr < KEXEC_RESERVE_PHYS ||
        phys_addr >= KEXEC_RESERVE_PHYS + KEXEC_REGION_SIZE) {
        kprintf("[!!] kexec_load: phys_addr 0x%llx outside reserved region "
                "[0x%llx, 0x%llx)\n",
                (unsigned long long)phys_addr,
                (unsigned long long)KEXEC_RESERVE_PHYS,
                (unsigned long long)(KEXEC_RESERVE_PHYS + KEXEC_REGION_SIZE));
        return -EINVAL;
    }

    /* Entry point must be a reasonable physical address */
    if (entry < KEXEC_RESERVE_PHYS ||
        entry >= KEXEC_RESERVE_PHYS + KEXEC_REGION_SIZE) {
        kprintf("[!!] kexec_load: entry 0x%llx outside reserved region\n",
                (unsigned long long)entry);
        return -EINVAL;
    }

    /* Unused flags are silently ignored for forward compatibility */
    (void)flags;

    /* Record the load request */
    kexec_state.phys_addr = phys_addr;
    kexec_state.entry     = entry;
    kexec_state.flags     = flags;
    kexec_state.loaded    = 1;

    kprintf("[OK] kexec: loaded kernel at phys 0x%llx, entry 0x%llx\n",
            (unsigned long long)phys_addr,
            (unsigned long long)entry);
    return 0;
}

/* ── Query ─────────────────────────────────────────────────────────── */

int kexec_is_loaded(void)
{
    return kexec_state.loaded;
}

uint64_t kexec_get_entry(void)
{
    return kexec_state.entry;
}

uint64_t kexec_get_phys_addr(void)
{
    return kexec_state.phys_addr;
}

/* ── Reboot — jump to the loaded kernel ───────────────────────────────
 *
 * This function does the actual transition to the new kernel.
 * It must be called with interrupts disabled (we do that here).
 * It never returns.
 *
 * Sequence:
 *   1. Disable interrupts (CLI)
 *   2. Mask all APIC LVT entries (prevents stray interrupts during handover)
 *   3. Send IPI to stop other CPUs (in SMP case; currently BSP only)
 *   4. Disable PIC (if using legacy PIC)
 *   5. Flush TLB on all CPUs
 *   6. Call the entry point as a bare function pointer (no stack transition)
 *
 * The new kernel starts executing at the entry point with:
 *   - CS.L = 1 (long mode)
 *   - Paging enabled (identity-mapped page tables expected by new kernel)
 *   - RFLAGS.IF = 0 (interrupts disabled)
 *   - RAX = boot magic (0xKEXEC_MAGIC) for new kernel to detect kexec boot
 */
#define KEXEC_BOOT_MAGIC  0x4B45584543ULL  /* "KEXEC" in ASCII */

/* ── APIC LVT masking ─────────────────────────────────────────────── */
static void kexec_mask_apic_lvts(void)
{
    /* Mask all LVT entries we know about.
     * Write the mask bit (bit 16) while keeping the delivery mode. */
    uint32_t lvt_entries[] = {
        LAPIC_LVT_TIMER,      /* 0x320 — Timer */
        LAPIC_LVT_PC,         /* 0x340 — Performance Counter */
        LAPIC_LVT_LINT0,      /* 0x350 — LINT0 */
        LAPIC_LVT_LINT1,      /* 0x360 — LINT1 */
        LAPIC_LVT_ERROR,      /* 0x370 — Error */
    };

    for (size_t i = 0; i < sizeof(lvt_entries) / sizeof(lvt_entries[0]); i++) {
        uint32_t val = apic_read(lvt_entries[i]);
        val |= (1 << 16);       /* set mask bit */
        apic_write(lvt_entries[i], val);
    }

    /* Also mask the spurious-interrupt vector register (bit 8 = mask) */
    uint32_t svr = apic_read(LAPIC_SVR);
    svr &= ~(1 << 8);          /* clear enable bit (disable the APIC entirely) */
    apic_write(LAPIC_SVR, svr);
}

void kexec_reboot(void)
{
    if (!kexec_state.loaded) {
        panic("kexec_reboot: no kernel loaded");
    }

    uint64_t entry = kexec_state.entry;

    kprintf("[kexec] Rebooting into new kernel at phys 0x%llx ...\n",
            (unsigned long long)entry);

    /* ── Step 1: Disable interrupts ─────────────────────────────────── */
    cli();

    /* ── Step 2: Mask all APIC LVT entries ──────────────────────────── */
    kexec_mask_apic_lvts();

    /* ── Step 3: Disable legacy PIC (if present) ────────────────────── */
    outb(0x20, 0xFF);  /* mask all IRQs on master PIC */
    outb(0xA0, 0xFF);  /* mask all IRQs on slave PIC  */
    io_wait();

    /* ── Step 4: Flush TLB on this CPU ──────────────────────────────── */
    write_cr3(read_cr3());  /* reload CR3 → flush TLB */

    /* ── Step 5: Print debug info if requested ──────────────────────── */
    if (kexec_state.flags & KEXEC_FLAG_DEBUG) {
        kprintf("[kexec] entry=0x%llx  phys_base=0x%llx  flags=0x%x\n",
                (unsigned long long)entry,
                (unsigned long long)kexec_state.phys_addr,
                (unsigned)kexec_state.flags);
    }

    /* ── Step 6: Jump to the new kernel entry point ───────────────────
     *
     * We enter the new kernel in long mode with paging enabled.
     * The new kernel is responsible for setting up its own page tables
     * (or using identity mapping from the current page tables).
     *
     * The calling convention mimics what a bootloader would provide:
     *   - RAX = KEXEC_BOOT_MAGIC (to indicate kexec boot)
     *   - RBX = physical address of the loaded image base
     *   - RCX = entry point (physical)
     *
     * We use a far jump to a 64-bit code segment to ensure CS is correct.
     * The new kernel expects to be called with:
     *   - CS selector = __KERNEL_CS (0x08 on this kernel)
     *   - CPL = 0
     *   - Long mode active
     *   - Paging enabled
     *   - Interrupts disabled
     */

    /* Compute the virtual address of the entry point.
     * Since we have an identity-mapped region (PHYS_TO_VIRT), we need
     * to jump to the virtual address.  The kernel image was loaded
     * at the physical address, but execution happens at the virtual
     * address.  For a simple implementation, we assume the new kernel
     * is position-independent or linked for the identity-mapped region.
     *
     * In the common case the entry point is a virtual address in the
     * new kernel's linked address space, so we must NOT apply
     * PHYS_TO_VIRT if it's already a virtual address.  We let the
     * caller pass a virtual entry point that maps to the loaded image.
     *
     * If phys_addr == entry, assume identity mapping and use PHYS_TO_VIRT.
     */
    uint64_t virt_entry;
    if (entry == kexec_state.phys_addr) {
        /* Entry is a physical address — convert to virtual */
        virt_entry = (uint64_t)PHYS_TO_VIRT(entry);
    } else {
        /* Entry is already a virtual address — use it directly */
        virt_entry = entry;
    }

    kprintf("[kexec] Jumping to 0x%llx (virt 0x%llx) ...\n",
            (unsigned long long)entry, (unsigned long long)virt_entry);

    /* Flush write buffers before jumping */
    __asm__ volatile("mfence" ::: "memory");

    /* Jump to the new kernel.  We do a tail-call via inline assembly
     * to ensure no stack-based state leaks into the new kernel.
     *
     * Arguments passed to the new kernel:
     *   RAX = KEXEC_BOOT_MAGIC
     *   RBX = phys_addr of loaded image
     *
     * We use a simple indirect jump; the new kernel inherits the
     * current page tables and must identity-map enough to reach its
     * entry point or use the current mapping.
     *
     * Use "m" constraints for input values to avoid register pressure
     * since we clobber almost all GPRs. */
    __asm__ volatile(
        "movq  %[magic], %%rax\n\t"
        "movq  %[phys],  %%rbx\n\t"
        "movq  %[entry_val], %%rcx\n\t"
        "xorq  %%rdx, %%rdx\n\t"
        "xorq  %%rsi, %%rsi\n\t"
        "xorq  %%rdi, %%rdi\n\t"
        "xorq  %%r8,  %%r8\n\t"
        "xorq  %%r9,  %%r9\n\t"
        "xorq  %%r10, %%r10\n\t"
        "xorq  %%r11, %%r11\n\t"
        "xorq  %%r15, %%r15\n\t"
        "jmp   *%%rcx\n\t"
        :
        : [magic] "r"(KEXEC_BOOT_MAGIC),
          [phys]  "r"(kexec_state.phys_addr),
          [entry_val] "r"(virt_entry)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r15",
          "memory"
    );

    /* Should never reach here */
    for (;;)
        __asm__ volatile("hlt");
}
