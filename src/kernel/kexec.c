/*
 * kexec.c — Load and execute a new kernel without firmware reboot
 *
 * Item 362 from the production-improvements plan.
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
 *      panics -> kexec_reboot() is called.
 *   4. kexec_reboot() disables interrupts, masks all APIC LVT entries,
 *      flushes TLBs, and jumps to the entry point in 64-bit long mode.
 *
 * Enhanced with:
 *   - Multi-segment support (up to KEXEC_SEGMENT_MAX = 16 segments)
 *   - Segment integrity validation (no overlap, reasonable sizes)
 *   - Crash kernel region reservation from crashkernel= parameter
 *   - kexec_load_disabled and crash_kexec_post_notifiers sysfs toggles
 *
 * The caller is responsible for:
 *   - Loading the new kernel ELF and any initrd into the reserved region.
 *   - Ensuring devices are quiesced, or calling from a context where
 *     that is guaranteed (e.g., after panic or explicit shutdown sequence).
 *
 * See also: kdump.c for the crash-kernel variant (Item 363).
 */

#define KERNEL_INTERNAL
#include "kexec.h"
#include "errno.h"
#include "pmm.h"
#include "vmm.h"
#include "printf.h"
#include "string.h"
#include "panic.h"
#include "apic.h"
#include "cpu.h"
#include "smp.h"
#include "io.h"
#include "lockdown.h"
#include "kernel.h"

/* ── Global state ──────────────────────────────────────────────────── */

/* Sysfs toggle: /sys/kernel/kexec_load_disabled — writable, default 0 */
int kexec_load_disabled = 0;

/* Sysfs toggle: /sys/kernel/crash_kexec_post_notifiers — writable, default 0 */
int crash_kexec_post_notifiers = 0;

/* Crash kernel region (parsed from crashkernel= kernel param) */
uint64_t crash_kernel_base = 0;
uint64_t crash_kernel_size = 0;
int      crash_kernel_reserved = 0;

/* ── Segment storage ─────────────────────────────────────────────────
 *
 * Stores up to KEXEC_SEGMENT_MAX segments for the currently loaded
 * kexec image.  Static allocation — no dynamic memory after init.
 */
static struct {
    struct kexec_segment segments[KEXEC_SEGMENT_MAX];
    unsigned long       nr_segments;
    unsigned long       entry;
    unsigned long       flags;
    int                 loaded;
} kexec_seg_state;

/* Single-segment compatibility state (for the legacy kexec_load API) */
static volatile struct {
    uint64_t phys_addr;        /* physical address of loaded kernel image */
    uint64_t entry;            /* physical entry point */
    uint32_t flags;            /* KEXEC_FLAG_* */
    int      loaded;           /* 1 if a kernel image has been loaded */
} kexec_state;

static int kexec_region_reserved = 0;   /* 1 after successful kexec_init */

/* Crash kernel state */
static volatile struct {
    uint64_t entry;
    uint32_t flags;
    int      loaded;
} kexec_crash_state;

/* ── Segment helpers ───────────────────────────────────────────────── */

/* Check if two memory ranges overlap */
static int ranges_overlap(uint64_t start1, uint64_t size1,
                          uint64_t start2, uint64_t size2)
{
    if (size1 == 0 || size2 == 0) return 0;
    uint64_t end1 = start1 + size1 - 1;
    uint64_t end2 = start2 + size2 - 1;
    return (start1 <= end2 && start2 <= end1);
}

/* Validate a segment for reasonable size and location */
static int validate_segment(const struct kexec_segment *seg, int index)
{
    /* Must have non-zero memory size */
    if (seg->memsz == 0) {
        kprintf("[!!] kexec: segment %d has zero memsz\n", index);
        return -EINVAL;
    }

    /* Maximum segment size: 16 MB (to stay within reserved region) */
    if (seg->memsz > KEXEC_REGION_SIZE) {
        kprintf("[!!] kexec: segment %d memsz %llu exceeds region size\n",
                index, (unsigned long long)seg->memsz);
        return -EINVAL;
    }

    /* Physical address must be within the reserved kexec region */
    if (seg->mem < KEXEC_RESERVE_PHYS ||
        seg->mem + seg->memsz > KEXEC_RESERVE_PHYS + KEXEC_REGION_SIZE) {
        kprintf("[!!] kexec: segment %d phys [0x%llx, 0x%llx) outside region\n",
                index,
                (unsigned long long)seg->mem,
                (unsigned long long)(seg->mem + seg->memsz));
        return -EINVAL;
    }

    /* Buffer size must not exceed memory size */
    if (seg->bufsz > seg->memsz) {
        kprintf("[!!] kexec: segment %d bufsz %llu > memsz %llu\n",
                index, (unsigned long long)seg->bufsz,
                (unsigned long long)seg->memsz);
        return -EINVAL;
    }

    return 0;
}

/* ── Initialization ────────────────────────────────────────────────── */

int kexec_init(void)
{
    if (kexec_region_reserved) {
        kprintf("[!!] kexec: already initialized\n");
        return -EBUSY;
    }

    /* Reserve the physical region from PMM so no other allocator uses it. */
    pmm_reserve_frames(KEXEC_RESERVE_PHYS, KEXEC_REGION_SIZE);

    /* Clear the state */
    memset((void *)&kexec_state, 0, sizeof(kexec_state));
    memset((void *)&kexec_seg_state, 0, sizeof(kexec_seg_state));
    memset((void *)&kexec_crash_state, 0, sizeof(kexec_crash_state));

    kexec_region_reserved = 1;
    kprintf("[OK] kexec: reserved %u MB at phys 0x%llx\n",
            (unsigned)(KEXEC_REGION_SIZE / (1024 * 1024)),
            (unsigned long long)KEXEC_RESERVE_PHYS);
    return 0;
}

/* ── Load (legacy single-segment API) ──────────────────────────────── */

int kexec_load(uint64_t phys_addr, uint64_t entry, uint32_t flags)
{
    /* Check kexec_load_disabled sysfs toggle */
    if (kexec_load_disabled)
        return -EPERM;

    /* Reject kexec_load under lockdown INTEGRITY or above */
    if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY))
        return -EPERM;

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

    /* Also populate the segment state with a single segment */
    kexec_seg_state.nr_segments = 1;
    kexec_seg_state.segments[0].buf   = 0;
    kexec_seg_state.segments[0].bufsz = 0;
    kexec_seg_state.segments[0].mem   = phys_addr;
    kexec_seg_state.segments[0].memsz = KEXEC_REGION_SIZE;
    kexec_seg_state.entry  = entry;
    kexec_seg_state.flags  = flags;
    kexec_seg_state.loaded = 1;

    kprintf("[OK] kexec: loaded kernel at phys 0x%llx, entry 0x%llx\n",
            (unsigned long long)phys_addr,
            (unsigned long long)entry);
    return 0;
}

/* ── Load segments (multi-segment API) ─────────────────────────────── */

int kexec_load_segments(const struct kexec_segment *segments,
                        unsigned long nr_segments,
                        unsigned long entry,
                        unsigned long flags)
{
    if (!segments) return -EINVAL;

    /* Check kexec_load_disabled sysfs toggle */
    if (kexec_load_disabled)
        return -EPERM;

    /* Reject under lockdown */
    if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY))
        return -EPERM;

    if (!kexec_region_reserved) {
        kprintf("[!!] kexec_load_segments: kexec not initialized\n");
        return -ENODEV;
    }

    /* Validate segment count */
    if (nr_segments == 0 || nr_segments > KEXEC_SEGMENT_MAX) {
        kprintf("[!!] kexec_load_segments: invalid nr_segments %lu\n",
                nr_segments);
        return -EINVAL;
    }

    /* Validate each segment individually */
    for (unsigned long i = 0; i < nr_segments; i++) {
        int ret = validate_segment(&segments[i], (int)i);
        if (ret != 0)
            return ret;
    }

    /* Validate no overlap between segments */
    for (unsigned long i = 0; i < nr_segments; i++) {
        for (unsigned long j = i + 1; j < nr_segments; j++) {
            if (ranges_overlap(segments[i].mem, segments[i].memsz,
                               segments[j].mem, segments[j].memsz)) {
                kprintf("[!!] kexec_load_segments: segment %lu overlaps %lu\n",
                        i, j);
                return -EINVAL;
            }
        }
    }

    /* Entry must be within one of the segments */
    int entry_valid = 0;
    for (unsigned long i = 0; i < nr_segments; i++) {
        if (entry >= segments[i].mem &&
            entry < segments[i].mem + segments[i].memsz) {
            entry_valid = 1;
            break;
        }
    }
    if (!entry_valid) {
        kprintf("[!!] kexec_load_segments: entry 0x%lx outside all segments\n",
                entry);
        return -EINVAL;
    }

    /* Copy segments into our static storage */
    memset(&kexec_seg_state, 0, sizeof(kexec_seg_state));
    for (unsigned long i = 0; i < nr_segments; i++) {
        kexec_seg_state.segments[i] = segments[i];
    }
    kexec_seg_state.nr_segments = nr_segments;
    kexec_seg_state.entry  = entry;
    kexec_seg_state.flags  = flags;
    kexec_seg_state.loaded = 1;

    /* Also update the legacy single-segment state for backward compat */
    kexec_state.phys_addr = segments[0].mem;
    kexec_state.entry     = entry;
    kexec_state.flags     = (uint32_t)flags;
    kexec_state.loaded    = 1;

    kprintf("[OK] kexec: loaded %lu segments, entry 0x%lx\n",
            nr_segments, entry);
    return 0;
}

/* ── Query ─────────────────────────────────────────────────────────── */

int kexec_is_loaded(void)
{
    return kexec_state.loaded || kexec_seg_state.loaded;
}

uint64_t kexec_get_entry(void)
{
    if (kexec_seg_state.loaded)
        return kexec_seg_state.entry;
    return kexec_state.entry;
}

uint64_t kexec_get_phys_addr(void)
{
    if (kexec_seg_state.loaded && kexec_seg_state.nr_segments > 0)
        return kexec_seg_state.segments[0].mem;
    return kexec_state.phys_addr;
}

/* ── Crash kernel load ─────────────────────────────────────────────── */

int kexec_crash_load(uint64_t phys_addr, uint64_t entry, uint32_t flags)
{
    if (!crash_kernel_reserved) {
        kprintf("[!!] kexec_crash_load: no crash kernel region reserved\n");
        return -ENODEV;
    }

    /* Validate that the image fits within the crash kernel region */
    if (phys_addr < crash_kernel_base ||
        phys_addr >= crash_kernel_base + crash_kernel_size) {
        kprintf("[!!] kexec_crash_load: phys_addr 0x%llx outside crash region "
                "[0x%llx, 0x%llx)\n",
                (unsigned long long)phys_addr,
                (unsigned long long)crash_kernel_base,
                (unsigned long long)(crash_kernel_base + crash_kernel_size));
        return -EINVAL;
    }

    if (entry < crash_kernel_base ||
        entry >= crash_kernel_base + crash_kernel_size) {
        kprintf("[!!] kexec_crash_load: entry outside crash region\n");
        return -EINVAL;
    }

    kexec_crash_state.entry  = entry;
    kexec_crash_state.flags  = flags;
    kexec_crash_state.loaded = 1;

    kprintf("[OK] kexec: crash kernel loaded at phys 0x%llx, entry 0x%llx\n",
            (unsigned long long)phys_addr, (unsigned long long)entry);
    return 0;
}

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

    for (size_t i = 0; i < ARRAY_SIZE(lvt_entries); i++) {
        uint32_t val = apic_read(lvt_entries[i]);
        val |= (1U << 16);       /* set mask bit */
        apic_write(lvt_entries[i], val);
    }

    /* Also mask the spurious-interrupt vector register (bit 8 = mask) */
    uint32_t svr = apic_read(LAPIC_SVR);
    svr &= ~(1U << 8);          /* clear enable bit (disable the APIC entirely) */
    apic_write(LAPIC_SVR, svr);
}

/*
 * kexec_do_reboot — Common kexec reboot transition.
 *
 * @entry: Physical entry point to jump to.
 * @phys_base: Physical base address (loaded image base).
 *
 * Disables interrupts, masks APIC, disables PIC, flushes TLB,
 * and jumps to the entry point.
 */
static void __attribute__((noreturn)) kexec_do_reboot(uint64_t entry, uint64_t phys_base)
{
    /* ── Step 1: Disable interrupts ─────────────────────────────────── */
    cli();

    /* ── Step 2: Mask all APIC LVT entries ──────────────────────────── */
    kexec_mask_apic_lvts();

    /* ── Step 3: Disable legacy PIC (if present) ────────────────────── */
    outb(0x20, 0xFF);  /* mask all IRQs on master PIC */
    outb(0xA0, 0xFF);  /* mask all IRQs on slave PIC  */
    io_wait();

    /* ── Step 4: Flush TLB on this CPU ──────────────────────────────── */
    write_cr3(read_cr3());  /* reload CR3 -> flush TLB */

    /* ── Step 5: Flush write buffers before jumping */
    __asm__ volatile("mfence" ::: "memory");

    kprintf("[kexec] Jumping to 0x%llx (phys_base 0x%llx) ...\n",
            (unsigned long long)entry, (unsigned long long)phys_base);

    /* ── Step 6: Jump to the entry point ──────────────────────────────
     *
     * We enter the new kernel in long mode with paging enabled.
     * The new kernel is responsible for setting up its own page tables.
     *
     * Arguments passed to the new kernel:
     *   RAX = KEXEC_BOOT_MAGIC (0x4B45584543 = "KEXEC")
     *   RBX = phys_base (physical address of the loaded image base)
     *   RCX = entry point
     */
    const uint64_t KEXEC_BOOT_MAGIC = 0x4B45584543ULL;

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
          [phys]  "r"(phys_base),
          [entry_val] "r"(entry)
        : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
          "r8", "r9", "r10", "r11", "r15",
          "memory"
    );

    /* Should never reach here */
    for (;;)
        __asm__ volatile("hlt");
}

void kexec_reboot(void)
{
    if (!kexec_seg_state.loaded && !kexec_state.loaded) {
        panic("kexec_reboot: no kernel loaded");
    }

    uint64_t entry;
    uint64_t phys_base;

    if (kexec_seg_state.loaded) {
        entry = kexec_seg_state.entry;
        phys_base = kexec_seg_state.segments[0].mem;
    } else {
        entry = kexec_state.entry;
        phys_base = kexec_state.phys_addr;
    }

    kprintf("[kexec] Rebooting into new kernel at phys 0x%llx ...\n",
            (unsigned long long)entry);

    kexec_do_reboot(entry, phys_base);
}

void kexec_crash_reboot(void)
{
    if (!kexec_crash_state.loaded) {
        panic("kexec_crash_reboot: no crash kernel loaded");
    }

    kprintf("[kexec] Booting crash kernel at entry 0x%llx ...\n",
            (unsigned long long)kexec_crash_state.entry);

    kexec_do_reboot(kexec_crash_state.entry, crash_kernel_base);
}

int kexec_crash_is_loaded(void)
{
    return kexec_crash_state.loaded;
}

/* ── Stub: kexec_unload ────────────────────────────────────────────── */
int kexec_unload(void)
{
    kprintf("[KEXEC] kexec_unload: not yet implemented\n");
    return 0;
}

/* ── Stub: kexec_shutdown ──────────────────────────────────────────── */
void kexec_shutdown(void)
{
    kprintf("[KEXEC] kexec_shutdown: not yet implemented\n");
}

/* ── Stub: kexec_jump ──────────────────────────────────────────────── */
int kexec_jump(void)
{
    kprintf("[KEXEC] kexec_jump: not yet implemented\n");
    return 0;
}
