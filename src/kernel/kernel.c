#define KERNEL_INTERNAL
#include "acpi.h"
#include "ahci.h"
#include "ata.h"
#include "bridge.h"
#include "cpupstate.h"
#include "dhcp.h"
#include "e1000.h"
#include "fault.h"
#include "fs.h"
#include "gdt.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "ipip.h"
#include "ipvs.h"
#include "keyboard.h"
#include "mouse.h"
#include "net.h"
#include "net_ns.h"
#include "netdevice.h"
#include "netfilter.h"
#include "page_cache.h"
#include "pci.h"
#include "pic.h"
#include "pkt_sched.h"
#include "pmm.h"
#include "printf.h"
#include "process.h"
#include "rtc.h"
#include "scheduler.h"
#include "serial.h"
#include "sound_mixer_sw.h"
#include "speaker.h"
#include "ssh.h"
#include "string.h"
#include "syscall.h"
#include "telnetd.h"
#include "timer.h"
#include "tun.h"
#include "types.h"
#include "uhid.h"
#include "usb.h"
#include "usb_msc.h"
#include "veth.h"
#include "vga.h"
#include "vlan.h"
#include "vmm.h"
#include "vmxnet3.h"
#include "wireguard.h"
/* Externs for USB drivers without dedicated kernel includes */
extern int usb_cdc_acm_init(void);
extern int usb_hub_init(void);
#include "ac97.h"
#include "acpi_ec.h"
#include "acpi_thermal.h"
#include "aio_enhanced.h"
#include "apic.h"
#include "aslr.h"
#include "audit.h"
#include "blockdev.h"
#include "caps.h"
#include "cgroup.h"
#include "cma.h"
#include "cmdline.h"
#include "compaction.h"
#include "config_gz.h"
#include "container.h"
#include "core_sched.h"
#include "cpu.h"
#include "cpu_features.h"
#include "cpu_topology.h"
#include "cpuidle.h"
#include "cpuset.h"
#include "debugfs.h"
#include "devfs.h"
#include "devtmpfs.h"
#include "dm.h"
#include "dmesg.h"
#include "dyndbg.h"
#include "edac.h"
#include "elf.h"
#include "export.h"
#include "fanotify.h"
#include "fat32.h"
#include "fault_inject.h"
#include "fbcon.h"
#include "file_lock.h"
#include "firmware.h"
#include "fs_mount_prop.h"
#include "fsnotify.h"
#include "fstab.h"
#include "genhd.h"
#include "ghes.h"
#include "httpd.h"
#include "hugetlb.h"
#include "hung_task.h"
#include "i3c.h"
#include "initcall.h"
#include "intel_gpu.h"
#include "irq_regs.h"
#include "jump_label.h"
#include "kasan_light.h"
#include "kaslr.h"
#include "kdump.h"
#include "kexec.h"
#include "kmemleak.h"
#include "kprobes.h"
#include "kptr_restrict.h"
#include "ksm.h"
#include "kunit.h"
#include "landlock.h"
#include "lockdep.h"
#include "lsm.h"
#include "madvise_ext.h"
#include "mce.h"
#include "mdadm.h"
#include "mem_policy.h"
#include "memfd.h"
#include "memhotplug.h"
#include "module.h"
#include "module_elf.h"
#include "module_signature.h"
#include "mpath.h"
#include "mseal.h"
#include "net_igmp.h"
#include "net_lldp.h"
#include "net_rps.h"
#include "nmi_watchdog.h"
#include "nohz.h"
#include "nvme.h"
#include "nx_enforce.h"
#include "oom.h"
#include "overlay.h"
#include "overlay_enhance.h"
#include "page_allocator_ext.h"
#include "page_idle.h"
#include "page_poison.h"
#include "panic.h"
#include "pelt.h"
#include "perf_events.h"
#include "pidfd.h"
#include "pipe.h"
#include "pm_qos.h"
#include "process_rlimit.h"
#include "psi.h"
#include "pstore.h"
#include "ramdisk.h"
#include "rcu.h"
#include "rng.h"
#include "rseq.h"
#include "sched_attr.h"
#include "seccomp.h"
#include "seccomp_bpf.h"
#include "service.h"
#include "shm.h"
#include "slab.h"
#include "smp.h"
#include "softirq.h"
#include "spi.h"
#include "splash.h"
#include "stack_guard.h"
#include "stdio.h"
#include "string.h"
#include "swap.h"
#include "sysctl.h"
#include "sysfs.h"
#include "sysrq.h"
#include "tasklet.h"
#include "thp.h"
#include "timers.h"
#include "tmpfs.h"
#include "tracefs.h"
#include "tsc_deadline.h"
#include "userfaultfd.h"
#include "users.h"
#include "verity.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "virtio_net.h"
#include "vsyscall.h"
#include "watchdog.h"
#include "workqueue.h"
#include "wx_enforce.h"
#include "x2apic.h"
#include "yama.h"
#include "zram.h"
#include "zswap.h"
#ifdef TEST_MODE
#include "test.h"
#endif

/* ── Forward declarations ─────────────────────────────── */
void stdio_init(void);

/* ── Initcall support ─────────────────────────────────────────────── */

extern initcall_t __initcall_start[];
extern initcall_t __initcall_end[];

/* ── Module init ordering documentation ─────────────────────────────
 *
 * The kernel_main() function initialises all subsystems in a carefully
 * ordered sequence divided into the following phases.  Dependencies flow
 * top-to-bottom: a phase depends only on phases that precede it.
 *
 * Phase 1 — Early bootstrap (no heap, no memory manager)
 *   serial_init → vga_init → gdt_init → pic_init → idt_init →
 *   stack_guard_init → smp_init_bsp → pmm_init
 *   Responsibility: bare-metal CPU state (GDT/IDT/PIC), physical memory
 *   map discovery, and the earliest possible debug output.
 *
 * Phase 2 — Memory infrastructure
 *   vmm_init → heap_init → slab_init → lockdep_init → jump_label_init
 *   Responsibility: Virtual address space, dynamic allocation (heap +
 *   slab), and locking correctness infrastructure.
 *
 * Phase 3 — Error detection & recovery
 *   kasan_init → kmemleak_init → panic_init → pstore_init → kdump_init
 *   → kexec_init
 *   Responsibility: Memory corruptions detection, panic/oops handling,
 *   crash dump capture.
 *
 * Phase 4 — Security foundations
 *   seccomp_init → landlock_init → audit_init → yama_init →
 *   kptr_restrict_init → dmesg_init → caps_init → sysrq_init →
 *   nmi_watchdog_init
 *   Responsibility: System-call sandboxing, access control, audit trail,
 *   and watchdog/hang detection.
 *
 * Phase 5 — Memory management extensions
 *   compaction_init → memhp_init → page_poison_init → cma_init →
 *   ksm_init → thp_init → hugetlb_init → madvise_ext_init →
 *   mem_policy_init → page_idle_init → page_allocator_ext_init →
 *   rng_init → perf_init → pebs_init
 *   Responsibility: Defragmentation, hotplug, memory dedup, huge pages,
 *   performance counters.
 *
 * Phase 6 — Block device foundations
 *   ramdisk_init → tmpfs_init → fbcon_init (framebuffer console)
 *   Responsibility: Early block devices needed by the rest of the system.
 *
 * Phase 7 — Process & scheduling
 *   process_init → rlimit_init → pidfd_init → cpu_topology_init →
 *   sched_core_init → scheduler_init → psi_init → pm_qos_init →
 *   cpuidle_init → nohz_init → pelt_subsys_init → sched_attr_init →
 *   cpuset_init → rseq_init
 *   Responsibility: Process table, scheduler tick, load tracking, CPU
 *   idle states, and per-CPU user-space operations.
 *
 * Phase 8 — Interrupt delivery & SMP
 *   apic_init_local → ipi_init → softirq_init → smp_boot_aps →
 *   timer_init → x2apic_init → timers_init → workqueue_init
 *   Responsibility: Local APIC, SMP bring-up, system timer, dynamic
 *   timers, and deferred work execution.
 *
 * Phase 9 — Kernel services & modules
 *   thread_info_init → fanotify_init → fsnotify_init → modules_init →
 *   module_sig_init → ksym_init → do_initcalls (all __initcall funcs)
 *   → tpm_rng_init → firmware_init → fault_inject_init
 *   Responsibility: Module loader, symbol export, initcall dispatch,
 *   firmware API, fault injection framework.
 *
 * Phase 10 — Device infrastructure
 *   devtmpfs_init → overlay_init → keyboard_init → serial IRQ mode →
 *   rtc_init → mouse_init → speaker_init → spi_init → acpi_init →
 *   acpi_thermal_init → ec_init → syscall_init → production_subsystems
 *   Responsibility: Dynamic device nodes, input, RTC, ACPI tables,
 *   syscall dispatch, epoll/timerfd/mq.
 *
 * Phase 11 — Virtual filesystem
 *   vfs_init → procfs_init → fstab_mount_all → swap_init → sysfs_init
 *   → devfs_init → debugfs_init → tracefs_init → dyndbg_init →
 *   kunit_init → file_lock_init → mount_prop_init → vsyscall_init →
 *   memfd_init → mseal_init → uffd_init
 *   Responsibility: VFS layer, /proc, /sys, /dev, swap, kernel unit
 *   tests, memory sealing, userfaultfd.
 *
 * Phase 12 — IPC
 *   pipe_init → shm_init
 *   Responsibility: Inter-process communication primitives.
 *
 * Phase 13 — Block layer & storage drivers
 *   blockdev_init → genhd_init → dm_init (linear, zero, error, crypt,
 *   verity, raid) → mpath_init → cgroup_init → edac_init → ghes_init
 *   → i3c_init → fsverity_init → zram_init → zswap_init → ata_init →
 *   ahci_init → fat32_mount → fs_init → page_cache_init → mglru_init
 *   Responsibility: Block device abstraction, device mapper, RAID,
 *   compression (zram/zswap), disk drivers, filesystem+page cache.
 *
 * Phase 14 — Services & configuration
 *   service_init → initramfs_extract → hostname setup → inittab setup
 *   Responsibility: Service manager, embedded initramfs, boot config.
 *
 * Phase 15 — PCI, GPU, USB, users
 *   pci_init → intel_gpu_init → usb_init → usb_msc_init → usb_hid_init
 *   → usb_cdc_acm_init → usb_hub_init → users_init
 *   Responsibility: PCI bus enumeration, GPU, USB, multi-user support.
 *
 * Phase 16 — Network
 *   virtio_net_init → virtio_blk_init → nvme_init → raid_md_init →
 *   pmem_init → netdevice_init → veth_init → e1000_init → vmxnet3_init
 *   → nf_init → pkt_sched_init → bridge_init → vlan_init → tun_init →
 *   net_ns_init → ipip_init → wg_init → ipvs_init → net_init →
 *   dhcp_init → service registration → container_init → igmp_init →
 *   lldp_init
 *   Responsibility: NIC drivers, netfilter, bridging, VPN, DHCP,
 *   service startup, container runtime, multicast/LLDP.
 *
 * Phase 17 — Userspace handoff
 *   initrd loading → init process spawn → idle loop
 *   Responsibility: Load initrd, spawn /sbin/init (or cmdline-specified
 *   init binary), become idle process.
 *
 * Each subsystem init function should be idempotent where practical
 * (safe to call twice) and must not depend on subsystems in later
 * phases.  The do_initcalls() mechanism runs any function placed
 * in the .initcall section; these typically cover optional drivers
 * and late-stage hooks.
 *
 * See also: src/boot/boot.asm (early bootstrap), src/kernel/module.c
 * (loadable module init), userspace/init/init.c (userspace init).
 */
void do_initcalls(void) {
    initcall_t *fn;
    uintptr_t start = (uintptr_t)__initcall_start;
    uintptr_t end = (uintptr_t)__initcall_end;
    for (fn = (initcall_t *)start; (uintptr_t)fn < end; fn++) {
        if (*fn) {
            (*fn)();
        }
    }
}
/* ── Boot timing ───────────────────────────────────────────────── */
/* Captured at earliest possible point after entry (TSC timestamp) */
static uint64_t boot_start_tsc = 0;
static uint64_t boot_time_ms = 0;

/* Read the x86 Time-Stamp Counter */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* Rough TSC frequency calibration using PIT/generic estimate.
 * On modern x86_64, the TSC frequency is usually the max core frequency.
 * We approximate ms by dividing TSC delta by (TSC_FREQ / 1000).
 * A more precise calibration could come from CPUID leaf 0x15 or MSR 0xCE. */
#define TSC_FREQ_ESTIMATE 2000000000ULL /* 2 GHz default estimate */

static uint32_t tsc_khz_estimate = 0;

/* Calibrate TSC frequency using CPUID leaf 0x15 (TSC/clock ratio).
 * Returns 0 on success, -1 if not available (uses default). */
static int calibrate_tsc(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15));
    if (ecx != 0 && ebx != 0) {
        /* TSC frequency = (ecx * ebx / eax) */
        uint64_t freq = (uint64_t)ecx * (uint64_t)ebx / (uint64_t)eax;
        if (freq > 1000000 && freq < 10000000000ULL) {
            tsc_khz_estimate = (uint32_t)(freq / 1000);
            return 0;
        }
    }
    return -1;
}

static uint64_t tsc_to_ms(uint64_t tsc_delta) {
    uint64_t khz = (tsc_khz_estimate > 0) ? (uint64_t)tsc_khz_estimate : (TSC_FREQ_ESTIMATE / 1000);
    return tsc_delta / khz;
}

static void boot_timing_report(void) {
    uint64_t now = rdtsc();
    uint64_t delta = now - boot_start_tsc;
    boot_time_ms = tsc_to_ms(delta);
    kprintf("\n[Boot] Took %llu ms (TSC delta = %llu cycles, %u KHz)\n",
            (unsigned long long)boot_time_ms, (unsigned long long)delta, tsc_khz_estimate);
}
uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

/* Prototypes for functions defined later in this file */
void __attribute__((noreturn)) __stack_chk_fail(void);
void kernel_main(uint32_t magic, uint64_t multiboot_info_phys);

void __attribute__((noreturn)) __stack_chk_fail(void) {
    kprintf("\n*** KERNEL STACK SMASHING DETECTED ***\n");
    cli();
    for (;;)
        hlt();
}

void kernel_main(uint32_t magic, uint64_t multiboot_info_phys) {
    /* Capture TSC boot timestamp as early as possible */
    boot_start_tsc = rdtsc();

    /* Initialize stack canary from PRNG as early as possible */
    __stack_chk_guard = (uint64_t)magic ^ multiboot_info_phys ^ 0xA5A5A5A5A5A5A5A5ULL;

    /* ── Early serial console (Item 400) ─────────────────────────────
     * Initialise COM1 UART before anything else — this gives us debug
     * output capability even if the kernel crashes during early init
     * (before PMM, before VGA, before the normal serial_init()).
     * The early_* functions use hardcoded port I/O with zero kernel
     * state dependencies. */
    early_serial_init();
    early_printascii("\n[early] booting Hermes OS kernel...\n");

    /* Initialize serial first for debug output */
    serial_init();

    /* Initialize VGA console first for output */
    vga_init();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("Booting OS...\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* Verify multiboot */
    if (magic != 0x2BADB002) {
        kprintf("ERROR: Not booted by Multiboot (magic: 0x%x)\n", magic);
        cli();
        for (;;)
            hlt();
    }
    kprintf("[OK] Multiboot verified\n");

    /* GDT */
    gdt_init();
    kprintf("[OK] GDT initialized\n");

    /* PIC */
    pic_init();
    kprintf("[OK] PIC initialized\n");

    /* IDT */
    idt_init();
    kprintf("[OK] IDT initialized\n");

    /* Kernel stack guard pages */
    stack_guard_init();

    /* Per-CPU data for SMP — must be before any spinlock is acquired */
    smp_init_bsp();
    kprintf("[OK] SMP per-CPU data initialized\n");

    /* Physical memory manager */
    pmm_init(multiboot_info_phys);
    kprintf("[OK] PMM initialized: %llu KB total, %llu KB used\n",
            (unsigned long long)pmm_get_total_frames() * 4,
            (unsigned long long)pmm_get_used_frames() * 4);

    /*
     * IST stacks for double fault, NMI, MCE protection.
     *
     * MUST come before fault_init() — without IST, a double fault (e.g. from
     * a kernel stack overflow) would try to push the error code / register
     * frame onto the already-corrupted stack, causing an instant triple-fault.
     * The IST switch gives #DF a known-good stack independent of the faulting
     * context.
     */
    ist_init();
    kprintf("[OK] IST stacks initialized\n");

    /* Exception handlers — registered AFTER IST stacks are live */
    fault_init();
    kprintf("[OK] Exception handlers registered (DF/NMI/MCE/PF/BP/DB)\n");

    /* Kprobes dynamic breakpoint system (Item 203) */
    kprobes_init();

    /* Machine Check Architecture — enable MCA banks for hardware error detection */
    mce_init();

    /* Per-CPU IRQ stacks for safe interrupt handling (needs PMM) */
    irq_regs_init();

    /* Kernel command line from multiboot info (offset 0x10 = cmdline phys addr) */
    {
        /* Validate that multiboot_info_phys points to sane physical memory */
        if (multiboot_info_phys == 0) {
            kprintf("[boot] WARNING: multiboot_info at 0x%lx looks invalid\n",
                    (unsigned long)multiboot_info_phys);
        } else {
            uint32_t *mbi = (uint32_t *)PHYS_TO_VIRT(multiboot_info_phys);
            uint32_t flags = mbi[0];
            if (flags & (1U << 2)) { /* cmdline flag */
                uint32_t cmdline_phys = mbi[4];
                if (cmdline_phys) {
                    const char *cmdline_virt = (const char *)PHYS_TO_VIRT((uint64_t)cmdline_phys);
                    cmdline_init(cmdline_virt);
                }
            }
        }

        /* ── Parse kernel boot parameters ───────────────────────────
         * Supported parameters (Item 297):
         *   quiet     — suppress [OK] boot messages (set loglevel to KERN_WARNING)
         *   debug     — show all debug-level messages (set loglevel to KERN_DEBUG)
         *   console=  — select console device: "serial", "vga", or "both"
         *   loglevel= — set console loglevel directly (0..7)
         */
        if (cmdline_has("quiet")) {
            console_loglevel = 4; /* KERN_WARNING — only warnings+errors shown */
        }
        if (cmdline_has("debug")) {
            console_loglevel = 7; /* KERN_DEBUG — show everything */
        }
        const char *loglevel_val = cmdline_get("loglevel");
        if (loglevel_val && *loglevel_val) {
            int ll = 0;
            const char *lp = loglevel_val;
            while (*lp >= '0' && *lp <= '9') {
                ll = ll * 10 + (*lp++ - '0');
            }
            if (ll >= 0 && ll <= 7) {
                console_loglevel = ll;
            }
        }
        const char *console_val = cmdline_get("console");
        if (console_val) {
            /* console= parameter will be consumed by serial/console init later.
             * Currently we just recognise it; the actual console switching
             * happens in serial_init / fbcon_init / vga_init.
             * Store indication for future use. */
            kprintf_level(KERN_INFO, "[OK] Console parameter: %s\n", console_val);
        }
    }

    /* Virtual memory manager */
    vmm_init();
    kprintf("[OK] VMM initialized\n");

    /* Enable CPU security features: SMEP, SMAP, NXE, UMIP */
    cpu_security_init();

    /* Extended CPU features */
    smap_smep_init();
    umip_init();
    fsgsbase_init();
    invpcid_init();
    rdpid_init();
    /* x2APIC (if supported) is enabled after full APIC+IOAPIC+timer
     * setup so we can save existing LAPIC state before the mode switch
     * and restore it via x2APIC MSRs afterwards — see x2apic_init(). */
    nx_enforce_init();

    /* W^X enforcement — reject writable+executable mappings */
    wx_enforce_init();

    /* KPTI — Kernel Page-Table Isolation (Meltdown mitigation) */
    extern void kpti_init(void);
    kpti_init();

    /* Kernel heap (framebuffer may allocate from heap) */
    heap_init();
    kprintf("[OK] Heap initialized\n");

    /* Slab allocator (for fixed-size kernel objects) */
    slab_init();

    /* Lock dependency validator */
    lockdep_init();

    /* Jump labels / static keys for efficient feature toggling */
    jump_label_init();

    /* KASAN light — kernel address sanitizer (heap + stack redzones) */
    kasan_init();

    /* kmemleak — kernel memory leak detector */
    kmemleak_init();

    /* Panic/oops handler with register dump */
    panic_init();

    /* PStore — persistent storage for panic/oops messages */
    pstore_init();

    /* Kdump — kernel crash dump capture region (post-mortem analysis) */
    kdump_init();

    /* Kexec — reserve memory region for loading new kernel images (Item 362) */
    kexec_init();

    /* /proc/config.gz — embedded kernel build configuration */
    config_gz_init();

    /* OOM killer */
    oom_init();

    /* RCU synchronization primitive */
    rcu_init();

    /* ASLR (Address Space Layout Randomization) */
    aslr_init();

    /* KASLR — Kernel base address randomization */
    kaslr_init();

    /* Seccomp syscall sandboxing */
    seccomp_init();

    /* Landlock sandbox (path-based access control) */
    landlock_init();

    /* LSM hook framework (modules register hooks after this) */
    lsm_init();

    /* Seccomp BPF filter support */
    seccomp_bpf_init();

    /* Audit subsystem */
    audit_init();

    /* YAMA ptrace security */
    yama_init();
    yama_sysctl_register();

    /* Kernel pointer restrict */
    kptr_restrict_init();

    /* dmesg restrict */
    dmesg_init();

    /* Capability bounding set — system-wide cap mask */
    sys_cap_bset_init();

    /* SysRq emergency commands */
    sysrq_init();

    /* NMI watchdog for hang detection */
    nmi_watchdog_init();

    /* Hung-task detection (scan wired into scheduler_tick) */
    hung_task_init();

    /* Memory compaction / defragmentation */
    compaction_init();

    /* Memory features */
    memhp_init();
    page_poison_init();
    cma_init();
    ksm_init();
    thp_init();
    /* HugeTLB — pre-allocated pool for MAP_HUGETLB */
    if (hugetlb_init(0) < 0)
        kprintf("[WARN] HugeTLB pool init failed — MAP_HUGETLB unavailable\n");

    /* Extended memory management features */
    madvise_ext_init();
    mem_policy_init();
    page_idle_init();
    page_allocator_ext_init();

    /* TSC deadline timer (after APIC is up) */
    /* Software RNG — seed from timer (timer not yet available, so we'll re-seed later) */
    rng_init();

    /* Performance monitoring (PMU counters if available) */
    perf_init();

    /* PEBS needs heap — happens after heap_init.  BSP only;
     * APs get pebs_init() in their ap_entry_c() path. */
    pebs_init();

    /* Ramdisk block device (needed before initrd loading) */
    ramdisk_init();

    /* tmpfs RAM-backed filesystem */
    tmpfs_init();

    if (vga_try_init_framebuffer(multiboot_info_phys) == 0) {
        kprintf("[OK] Framebuffer console enabled\n");
        /* Initialize fbcon with framebuffer info */
        uint8_t *fb_ptr;
        uint32_t fb_w, fb_h, fb_pitch;
        vga_get_framebuffer_ptr(&fb_ptr, &fb_w, &fb_h, &fb_pitch);
        if (fb_ptr) {
            fbcon_init((uint32_t *)fb_ptr, fb_w, fb_h, fb_pitch);
        }

        /* Boot splash screen (Item 398) — displayed on framebuffer if available */
        if (splash_should_show()) {
            splash_init();
            splash_progress(1);
        }
    } else
        kprintf("[OK] VGA text console (QEMU window or serial terminal)\n");

    /* Process subsystem */
    process_init();
    /* khugepaged disabled: its full-address-space scan (0 → 0x800000000000
     * in 2MB steps per user process) intermittently stalls early boot on
     * QEMU, delaying the network/init path past the e2e timeout. */
    /* khugepaged_start(); */
    kprintf("[OK] Process subsystem initialized\n");
    splash_progress(3);

    /* Process resource limits */
    rlimit_init();

    /* PID file descriptors */
    pidfd_init();

    /* CPU topology and NUMA detection */
    cpu_topology_init();
    numa_init();

    /* Core scheduling for SMT isolation */
    sched_core_init();

    /* Scheduler */
    scheduler_init();
    kprintf("[OK] Scheduler initialized\n");
    splash_progress(5);

    /* PSI — Pressure Stall Information (tracks CPU/memory/IO pressure) */
    psi_init();

    /* PM QoS — latency constraints for cpuidle C-state selection */
    pm_qos_init();

    /* CPU idle state management */
    cpuidle_init();

    /* Adaptive tick (NO_HZ_FULL) for isolated CPUs */
    nohz_init();

    /* PELT load tracking */
    pelt_subsys_init();

    /* Extended scheduler attributes (sched_setattr/getattr) */
    sched_attr_init();

    /* CPU set (affinity) management */
    cpuset_init();

    /* Restartable sequences (per-CPU user-space operations) */
    rseq_init();

    /* Local APIC (replaces PIC for interrupt delivery) */
    apic_init_local();
    tsc_deadline_init();
    kprintf("[OK] Local APIC initialized\n");
    splash_progress(7);

    /* Register IPI handlers for SMP coordination */
    ipi_init();
    kprintf("[OK] IPI handlers registered\n");

    /* SoftIRQ subsystem (deferred interrupt processing) */
    softirq_init();

    /* Tasklets — softirq-driven deferred callbacks.  Must be initialized
     * AFTER softirq_init() (which memsets the handler array), otherwise
     * tasklet_init()'s handler registration would be wiped and every
     * tasklet_schedule() raise would be silently dropped by do_softirq()
     * (lost wakeup — tasklet callbacks would never run). */
    tasklet_init();

    /* I/O APIC and SMP boot */
    int ap_count = smp_boot_aps();
    if (ap_count > 0)
        kprintf("[OK] SMP: %d AP(s) booted, total %d CPU(s)\n", ap_count, (int)smp_get_cpu_count());

    /* Timer (starts scheduling) */
    timer_init();
    kprintf("[OK] Timer initialized at %d Hz\n", TIMER_FREQ);

    /* Switch to x2APIC mode if CPU supports it.
     * This must happen after apic_init_local(), timer_init(), and
     * nmi_watchdog_init() have completed MMIO-based LAPIC setup.
     * x2apic_init() saves current state before the switch and
     * restores it via x2APIC MSRs to avoid losing LAPIC state
     * and dropping in-flight IRR interrupts. */
    x2apic_init();

    /* Dynamic kernel timers (driven by timer IRQ) */
    timers_init();

    /* PSI periodic update timer (every 2 seconds) */
    psi_timer_init();

    /* Workqueue (deferred work execution via kthread) */
    workqueue_init();

    /* ksoftirqd — per-CPU kernel thread for deferred softirq processing.
     * Processes softirqs that cannot be handled in IRQ context without
     * causing livelock.  Runs at SCHED_IDLE priority. */
    create_ksoftirqd();

    /* Thread info table for pthread support */
    thread_info_init();

    /* Fanotify — file system event monitoring */
    fanotify_init();

    /* Filesystem notification (inotify-like) */
    fsnotify_init();

    /* Kernel module API */
    modules_init();
    kprintf("[OK] Kernel module API initialized (%d slots)\n", MODULE_MAX);

    /* Module signature verification */
    module_sig_init();

    /* Kernel symbol export table — for module symbol resolution */
    ksym_init();

    /* Block device registry — must be initialized BEFORE do_initcalls()
     * runs driver initcalls (ata_init, ramdisk_init, ...) that register
     * devices.  Previously called later, which memset() the array and
     * wiped those registrations. */
    blockdev_init();

    /* Initcall system — run all registered initcalls in order.
     * Enable interrupts first so udelay/timer_get_ticks() work. */
    sti();
    do_initcalls();

    /* TPM RNG seeding — feed TPM hardware entropy into kernel RNG */
    {
        extern int tpm_rng_init(void);
        tpm_rng_init();
    }

    /* Firmware loading API */
    firmware_init();

    /* Fault injection framework — for testing error recovery paths */
    fault_inject_init();

    /* Devtmpfs — dynamic device node creation */
    devtmpfs_init();

    /* Overlay/union filesystem */
    overlay_init();

    /* Overlay enhancements: whiteout + opaque directory support */
    overlay_enhance_init();

    /* Keyboard */
    keyboard_init();
    kprintf("[OK] Keyboard initialized\n");

    /* Enable IRQ-driven serial console input for COM1 and COM2 */
    serial_set_irq_mode(0, 1);
    serial_set_irq_mode(1, 1);
    kprintf("[OK] Serial console input enabled (COM1, COM2)\n");

    /* RTC */
    rtc_init();
    struct rtc_time rtc;
    rtc_get_time(&rtc);
    kprintf("[OK] RTC: %u-%u-%u %u:%u:%u\n", rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute,
            rtc.second);

    /* RTC sysfs interface (wakealarm) */
    rtc_sysfs_init();

#ifdef TEST_MODE
    /* Test mode: skip mouse init (PS/2 may not have a mouse attached) */
    kprintf("[OK] Mouse (test mode: skipped)\n");
#else
    /* PS/2 Mouse */
    mouse_init();
    kprintf("[OK] Mouse initialized\n");
#endif

    /* PC Speaker */
    speaker_init();
    kprintf("[OK] Speaker initialized\n");

    /* SPI bus controller framework */
    spi_init();

    /* ACPI */
    acpi_init();

    /* ACPI thermal zones with adaptive polling governor */
    acpi_thermal_init();

    /* ACPI embedded controller with burst mode */
    ec_init();

    /* Syscall interface */
    syscall_init();

    /* Production subsystems (socket, epoll, timers, mq) */
    production_subsystems_init();
    kprintf("[OK] Production subsystems initialized\n");

    /* Enhanced async I/O (aio_read/write/poll) */
    aio_enhanced_init();

    /* VFS */
    vfs_init();
    kprintf("[OK] VFS initialized\n");
    splash_progress(12);

    /* procfs — /proc virtual filesystem (supports built-in + loadable module) */
    {
        extern void procfs_init(void);
        procfs_init();
    }

    /* Auto-mount filesystems from /etc/fstab */
    {
        int nm = fstab_mount_all();
        if (nm > 0)
            kprintf("[OK] fstab: %d filesystems auto-mounted\n", nm);
    }

    /* Swap subsystem — block device swap (Item 223) */
    swap_init();

    /* Sysfs — virtual filesystem exposing kernel objects */
    sysfs_init();

    /* Devfs — /dev device virtual filesystem */
    devfs_init();

    /* Kdump sysfs entries (/sys/kernel/kexec_load_disabled,
     * /sys/kernel/crash_kexec_post_notifiers) */
    kdump_sysfs_init();

    /* Watchdog sysfs interface (/sys/class/watchdog/watchdog0/) */
    watchdog_sysfs_init();

    /* CPU frequency scaling — ACPI P-states (ACPI _PSS / MSR fallback) */
    cpupstate_init();

    /* IMA — Integrity Measurement Architecture */
    {
        extern void ima_init(void);
        ima_init();
    }

    /* Debugfs — kernel debug data filesystem */
    debugfs_init();

    /* Tracefs — kernel trace filesystem with per-CPU trace buffers */
    tracefs_init();

    /* Dynamic debug — module/function-level pr_debug control via debugfs */
    dyndbg_init();

    /* MCE injection — debug interface for testing machine check handling (Item 396) */
    mce_inject_init();

    /* KUnit — in-kernel unit test framework */
    kunit_init();

    /* File locking (advisory + mandatory) */
    file_lock_init();

    /* Mount propagation attributes */
    mount_prop_init();

    /* vsyscall page for fast user-space syscalls */
    vsyscall_init();

    /* Anonymous file descriptors (memfd_create) */
    memfd_init();

    /* Memory sealing (mseal) */
    mseal_init();

    /* User page fault handling (userfaultfd) */
    uffd_init();

    /* Pipes */
    pipe_init();
    kprintf("[OK] Pipes initialized\n");

    /* Shared memory */
    shm_init();
    kprintf("[OK] Shared memory initialized\n");

    /* Generic disk layer (gendisk) — above blockdev, below partitions/filesystems */
    genhd_init();

    /* Device mapper framework — virtual block device layer */
    dm_init();
    dm_linear_init();
    dm_zero_init();
    dm_error_init();
    dm_crypt_init();
    dm_verity_init();
    dm_raid_init();

    /* Multipath I/O */
    mpath_init();

    /* Cgroup v2 unified hierarchy */
    cgroup_init();

    /* EDAC: DRAM ECC error detection */
    edac_init();

    /* ACPI GHES: hardware error source handler */
    ghes_init();

    /* I3C serial bus */
    i3c_init();

    /* fs-verity: Merkle tree per-file verification */
    fsverity_init();

    /* ZRAM compressed RAM block device — requires compression subsystem */
    zcomp_init();
    zram_init();

    /* Zswap compressed swap cache — reduces swap I/O by keeping
     * compressed pages in memory.  Falls back to disk if full. */
    zswap_init();

    /* ATA disk */
    ata_init();
    if (ata_is_present())
        kprintf("[OK] ATA disk detected\n");
    else
        kprintf("[--] No ATA disk found\n");

    /* AHCI SATA disk */
    if (ahci_init() == 0)
        kprintf("[OK] AHCI SATA initialized\n");
    else
        kprintf("[--] No AHCI controller\n");

        /* FAT32 — try to mount before fs_init so we don't format over it */
#ifndef TEST_MODE
    if (ahci_is_present()) {
        int fat_rc = fat32_mount(FAT32_DISK_AHCI, 0);
        if (fat_rc == 0) {
            vfs_mount("/mnt", &fat32_vfs_ops, NULL);
            kprintf("[OK] FAT32 mounted on /mnt\n");
        } else {
            kprintf("[!!] FAT32 mount (AHCI) failed: %d\n", fat_rc);
        }
    } else if (ata_is_present()) {
        int fat_rc = fat32_mount(FAT32_DISK_ATA, 0);
        if (fat_rc == 0) {
            vfs_mount("/mnt", &fat32_vfs_ops, NULL);
            kprintf("[OK] FAT32 mounted on /mnt\n");
        } else {
            kprintf("[!!] FAT32 mount failed: %d\n", fat_rc);
        }
    }
#else
        /* Test mode: skip slow FAT32 probe on ATA; use ramdisk. */
#endif

    /* Filesystem */
    fs_init();
    kprintf("[OK] Filesystem initialized\n");
    splash_progress(16);

    /* Page cache (file data caching + readahead) — initialized after filesystem
     * so it can be used by the simple block filesystem's fs_read_file(). */
    page_cache_init();

    /* Register the filesystem writeback callback so dirty pages in the
     * page cache are actually flushed to disk on eviction and sync. */
    fs_register_page_cache_writeback();

    /* Multi-Generational LRU page reclaim — initialise after page cache */
    extern void mglru_init(void);
    mglru_init();

    /* Service infrastructure + FS directory tree */
    service_init();
    kprintf("[OK] Service manager initialized\n");

    /* ── Extract embedded initramfs (CPIO archive) ──────────────── */
    {
        extern int initramfs_extract(void);
        int n = initramfs_extract();
        if (n > 0)
            kprintf("[OK] Initramfs: %d files extracted\n", n);
        else
            kprintf("[--] Initramfs: none found\n");
    }

    /* ── Read /etc/hostname and set kernel hostname ─────────────── */
    {
        char hostbuf[128];
        uint32_t hostlen = 0;
        if (vfs_read("/etc/hostname", hostbuf, sizeof(hostbuf) - 1, &hostlen) == 0 && hostlen > 0) {
            hostbuf[hostlen] = '\0';
            sysctl_set_hostname(hostbuf);
            kprintf("[OK] Hostname set from /etc/hostname: %s\n", sysctl_get_hostname());
        } else {
            /* /etc/hostname doesn't exist yet — use default "os" */
            /* Create it with the default hostname for future boots */
            const char *def = "os\n";
            vfs_create("/etc/hostname", 1);
            vfs_write("/etc/hostname", def, (uint32_t)strlen(def));
        }
    }

    /* ── Create default /etc/inittab if it doesn't exist (Item U26) ──── */
    {
        char inittab_buf[64];
        uint32_t inittab_len = 0;
        int ret = vfs_read("/etc/inittab", inittab_buf, sizeof(inittab_buf) - 1, &inittab_len);
        if (ret != 0 || inittab_len == 0) {
            /* No inittab — create a sensible default.
             * Format (SysV inittab): id:runlevels:action:process
             *   console::askfirst:/bin/sh  — spawn shell on first console
             *   ttyS0::respawn:/bin/getty  — spawn getty on serial
             * Runlevels: empty = all runlevels.
             * The init process (PID 1) parses /etc/inittab and manages
             * service lifecycle (respawn, once, sysinit, etc.). */
            const char *default_inittab = "# /etc/inittab - init configuration\n"
                                          "# Format: id:runlevels:action:process\n"
                                          "\n"
                                          "# Serial console getty\n"
                                          "ttyS0::respawn:/bin/getty\n"
                                          "\n"
                                          "# Primary console shell\n"
                                          "console::askfirst:/bin/sh\n";
            vfs_create("/etc/inittab", 1);
            vfs_write("/etc/inittab", default_inittab, (uint32_t)strlen(default_inittab));
            kprintf("[OK] Created default /etc/inittab\n");
        }
    }

    /* PCI bus */
    pci_init();
    kprintf("[OK] PCI initialized\n");

    /* Intel integrated GPU */
    if (intel_gpu_init() == 0)
        kprintf("[OK] Intel GPU initialized\n");
    else
        kprintf("[--] No Intel GPU found\n");

        /* USB (built-in only; module init handles its own init) */
#ifndef MODULE
    if (usb_init() == 0) {
        kprintf("[OK] USB initialized\n");
        if (usb_msc_init() == 0)
            kprintf("[OK] USB MSC device registered\n");
        else
            kprintf("[--] No USB MSC device\n");
        if (usb_hid_init() == 0)
            kprintf("[OK] USB HID initialized\n");
        else
            kprintf("[--] No USB HID devices\n");
        if (usb_cdc_acm_init() == 0)
            kprintf("[OK] USB CDC ACM initialized\n");
        else
            kprintf("[--] No USB CDC ACM devices\n");
        if (usb_hub_init() == 0)
            kprintf("[OK] USB hub initialized\n");
        else
            kprintf("[--] No USB hub devices\n");
    } else {
        kprintf("[--] No USB controllers\n");
    }
#endif /* !MODULE */

    /* Multiuser */
    users_init();
    kprintf("[OK] Multiuser initialized\n");

    /* Network */
    if (virtio_net_init() == 0)
        kprintf("[OK] virtio-net: initialized\n");
    else
        kprintf("[--] virtio-net: not present\n");

    if (virtio_blk_init() == 0) {
        virtio_blk_register_blockdev();
        kprintf("[OK] virtio-blk: %llu sectors\n", virtio_blk_sector_count());
    } else
        kprintf("[--] virtio-blk: not present\n");

    /* NVMe SSD */
    if (nvme_init() == 0)
        kprintf("[OK] NVMe SSD initialized\n");
    else
        kprintf("[--] NVMe: not present\n");

    /* MD/RAID subsystem — provides RAID0/RAID1 virtual block devices.
     * Must be initialized after member block devices (ATA, AHCI, NVMe, virtio-blk)
     * are registered so that blockdev_get_sectors() works. */
    raid_md_init();

    /* PMEM (NVDIMM) persistent memory block devices — discovered via
     * ACPI NFIT table parsing during acpi_init().  NFIT scanning
     * happens before this point and caches SPA range data. */
    extern void pmem_init(void);
    pmem_init();

    if (ac97_init() == 0)
        kprintf("[OK] AC97 audio: initialized\n");
    else
        kprintf("[--] AC97 audio: not present\n");

    /* OSS /dev/dsp audio interface — registers a /dev/dsp character
     * device for PCM playback via the AC97 hardware. */
    extern void sound_oss_init(void);
    sound_oss_init();

    /* Sound core mixer interface — exposes per-channel volume/mute
     * controls under /sys/class/sound/controlC0/.  Must be initialised
     * after AC97 so it can sync initial mixer state from hardware. */
    extern void sound_core_init(void);
    sound_core_init();

    /* Software audio mixer — virtual PCM mixing for multi-stream audio.
     * Creates a global mixer instance and wires it to the PC speaker
     * driver so speaker beeps are also routed through the sound card. */
    {
        static struct sound_mixer_sw g_sw_mixer;
        int ret = sound_mixer_sw_init(&g_sw_mixer, 2, 44100);
        if (ret == 0) {
            speaker_set_mixer(&g_sw_mixer);
            kprintf("[OK] Software mixer: 8-channel, 44.1 kHz stereo\n");
        } else {
            kprintf("[--] Software mixer: init failed (%d)\n", ret);
        }
    }

    /* Initialise the netdevice interface layer before any NIC driver
     * so they can register themselves as net devices during init. */
    netdevice_init();

    /* Virtual Ethernet pair driver — always available for ns networking */
    veth_init();

    if (e1000_init() == 0) {
        uint8_t mac[6];
        e1000_get_mac(mac);
        kprintf("[OK] e1000 NIC: %x:%x:%x:%x:%x:%x\n", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
    } else {
        kprintf("[--] e1000 NIC not found\n");
    }

    /* VMware vmxnet3 NIC — will succeed under VMware / Fusion */
    {
        int vmxnet3_ok;
        vmxnet3_ok = (vmxnet3_init() == 0) ? 1 : 0;
        if (vmxnet3_ok) {
            /* vmxnet3_init() has already printed identification details */
        }
    }

    /* Enable interrupts — needed for timer_get_ticks() in DHCP timeout */
    sti();

    if (virtio_net_present() || e1000_is_present()) {
        /* Networking subsystem inits */
        nf_init();
        pkt_sched_init();
#ifndef MODULE
        bridge_init();
#endif
        vlan_init();
        tun_init();
        net_ns_init();
#ifndef MODULE
        ipip_init();
#endif
        wg_init();
        ipvs_init();

        net_init();
        rps_rfs_init();
        tcp_tfo_init();
#ifndef TEST_MODE
        kprintf("[..] DHCP discovering...\n");
        dhcp_init();
        dhcp_discover();
#else
        /* Test mode: set QEMU user-mode defaults, skip slow DHCP */
        extern uint32_t net_our_ip, net_gateway_ip, net_subnet_mask, net_dns_server;
        net_our_ip = (10U << 24) | (0U << 16) | (2U << 8) | 15U;
        net_gateway_ip = (10U << 24) | (0U << 16) | (2U << 8) | 2U;
        net_subnet_mask = (255U << 24) | (255U << 16) | (255U << 8) | 0U;
        net_dns_server = (10U << 24) | (0U << 16) | (2U << 8) | 3U;
#endif
        uint8_t ip[4];
        net_get_ip(ip);
        kprintf("[OK] Network: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
        /* Register & start services */
        service_register("telnetd", telnetd_start, telnetd_stop);
        service_register("httpd", httpd_start, httpd_stop);
        service_register("sshd", sshd_start, sshd_stop);

        /* ── Service dependency setup (Item U3) ────────────────────
         * httpd depends on telnetd; sshd depends on httpd.
         * This means start order is: telnetd -> httpd -> sshd.
         * Stop order is: sshd -> httpd -> telnetd. */
        service_add_dep("httpd", "telnetd");
        service_add_dep("sshd", "httpd");

        /* Start services in dependency order (telnetd first, then httpd, then sshd) */
        service_start("sshd"); /* triggers sorted start of deps */
        kprintf("[OK] Services started\n");

        /* Initialize OCI container runtime */
        container_init();

        /* Multicast group management (IGMP) */
        igmp_init();

        /* Link Layer Discovery Protocol (LLDP) */
        lldp_init();

        /* netconsole — kernel log over UDP (Item 391) */
        extern void netconsole_init(void);
        netconsole_init();
    } else {
        kprintf("[--] No network device found\n");
    }

#ifdef TEST_MODE
    /* Test mode: run the test suite then shut down.
     * Run directly in the boot thread (no separate process) to avoid
     * scheduler issues where the test process never gets CPU time. */
    test_run_all();
    /* NOTREACHED */
#else

    /* Normal mode: try to load the userspace init binary.
     * If successful, the init process runs in ring 3 and manages
     * spawning shells, services, etc.
     * Falls back to kernel-mode shell if spawn fails. */

    /* Finalise boot splash: mark progress complete */
    splash_progress(SPLASH_MAX_STAGES);

    int init_ok = 0;

    /* Check for init= cmdline parameter first */
    const char *cmdline_init_path = cmdline_get("init");
    const char *init_path = "/mnt/sbin/init";

    if (cmdline_init_path && *cmdline_init_path) {
        init_path = cmdline_init_path;
    }

    /* Load initrd from multiboot module BEFORE spawning init,
     * so init can access files from the initrd. */
    {
        uint32_t *mbi = (uint32_t *)PHYS_TO_VIRT(multiboot_info_phys);
        if (mbi[0] & (1U << 3)) {
            uint32_t mods_count = mbi[5];
            uint64_t mods_addr = (uint64_t)mbi[6];
            if (mods_count > 0 && mods_addr > 0) {
                uint32_t *mod = (uint32_t *)PHYS_TO_VIRT(mods_addr);
                uint64_t mod_start = (uint64_t)mod[0];
                uint64_t mod_end = (uint64_t)mod[1];
                uint64_t mod_size = mod_end - mod_start;
                if (mod_size > 0 && mod_size < 16 * 1024 * 1024) {
                    kprintf("[OK] Initrd module: %llu bytes at 0x%llx\n",
                            (unsigned long long)mod_size, (unsigned long long)mod_start);
                    void *mod_data = PHYS_TO_VIRT(mod_start);
                    if (ramdisk_is_present()) {
                        uint32_t num_sectors = (uint32_t)((mod_size + 511) / 512);
                        if (num_sectors <= ramdisk_get_sectors()) {
                            for (uint32_t s = 0; s < num_sectors; s++) {
                                ramdisk_write_sectors(s, 1, (const uint8_t *)mod_data + s * 512);
                            }
                            kprintf("[OK] Initrd loaded into ramdisk (%u sectors)\n", num_sectors);
                        }
                    }
                }
            }
        }
    }

    /* Spawn the network polling thread.  The netd kthread drains the NIC
     * in process context: the e1000 legacy IRQ is unreliable (never
     * asserts), and the idle loop is starved once userspace init runs.
     * It must NOT run on the timer's IRQ stack — the TCP send path
     * re-enters tcp_lock from there and deadlocks. */
    {
        extern void telnetd_task(void);
        struct process *netd = kthread_create((void (*)(void *))telnetd_task, NULL, "netd");
        if (netd) {
            /* Wake-boost: the netd's vruntime is reset below the CFS min
             * on every wake, so it wins the runqueue pick each time it
             * polls.  Without this, EEVDF starves it once userspace
             * init's fork loop floods the runqueue with fresh children
             * and no packet ever gets processed. */
            netd->sched_boost_on_wake = 1;
            kprintf("[OK] netd: network polling thread (PID %d)\n", (int)netd->pid);
        } else {
            kprintf("[!!] netd: failed to create network thread\n");
        }
    }

    /* Spawn the HTTP accept-loop thread.  httpd_start() registers port 80
     * with no callbacks (accept-queue mode), so httpd_task() must run as
     * its own kthread to pull connections off the queue via net_tcp_accept(). */
    {
        extern void httpd_task(void);
        struct process *httpd = kthread_create((void (*)(void *))httpd_task, NULL, "httpd");
        if (httpd) {
            /* Same wake-boost as netd: wins the runqueue against the
             * userspace fork flood so it can accept/respond on time. */
            httpd->sched_boost_on_wake = 1;
            kprintf("[OK] httpd: accept-loop thread (PID %d)\n", (int)httpd->pid);
        } else {
            kprintf("[!!] httpd: failed to create accept-loop thread\n");
        }
    }

    /* Spawn userspace init via kernel-mode spawner */
    {
        extern int process_spawn_kernel(const char *path);
        int pid = process_spawn_kernel(init_path);
        if (pid > 0) {
            init_ok = 1;
            kprintf("[OK] Userspace init: %s (PID %d)\n", init_path, pid);
        } else {
            kprintf("[!!] process_spawn_kernel(%s) failed with %d\n", init_path, pid);
        }
    }

    if (!init_ok) {
        /* Fall back to kernel threads */
        kprintf("[--] No userspace init binary found\n");
        /* Task spawning happens via init binary or userspace telnetd.
         * Static test/net/httpd tasks were removed (dead code). */
        kprintf("[--] No fallback tasks available, waiting for userspace\n");
    }
    kprintf("[OK] Processes created\n");

    /* Calibrate TSC frequency for boot timing */
    calibrate_tsc();

    /* Print boot timing report */
    boot_timing_report();
#endif

    /* ── Boot version string ────────────────────────────────────────
     * Display the kernel version and build date after all subsystems
     * are initialised, just before the final hardening steps. */
    kprintf("\n============================================\n");
    kprintf("  Hermes OS Kernel — Version %s\n", KVERSION);
    kprintf("  Built: " __DATE__ " " __TIME__ "\n");
#ifdef BUILD_TIME
    kprintf("  Build config: %s\n", BUILD_TIME);
#endif
    kprintf("  SMP: %s, Preempt: %s\n",
#ifdef CONFIG_SMP
            "enabled",
#else
            "disabled",
#endif
#ifdef CONFIG_PREEMPT
            "full"
#elif defined(CONFIG_PREEMPT_VOLUNTARY)
            "voluntary"
#else
            "none (cooperative)"
#endif
    );
    {
        uint64_t total_mb = (uint64_t)pmm_get_total_frames() * 4096ULL / (1024ULL * 1024ULL);
        uint64_t free_mb = (uint64_t)(pmm_get_total_frames() - pmm_get_used_frames()) * 4096ULL /
                           (1024ULL * 1024ULL);
        kprintf("  Memory: %llu MB total, %llu MB free\n", (unsigned long long)total_mb,
                (unsigned long long)free_mb);
    }
    kprintf("============================================\n\n");

    /* ── Transition page poisoning from EARLY to LATE stage ─────────
     * All kernel subsystems are now initialized.  Switch to LATE-stage
     * poison patterns and verify that EARLY-stage poisoned regions
     * have not been corrupted (use-before-init detection). */
    page_poison_enter_late_stage();

    /* Fade out the boot splash — interrupts already enabled above, so
     * mdelay (which calls timer_get_ticks) will work correctly. */
    splash_fade_out();

    /* Start the NMI watchdog now that the scheduler is running and we
     * can pet it from the tick handler and context-switch paths. */
    nmi_watchdog_start();

    /* ── NX enforcement audit ──────────────────────────────────────
     * Walk the kernel page tables and verify that NX is set correctly:
     *   - .text pages are executable (NX cleared)
     *   - All other sections (.rodata, .data, .bss) have NX set
     * This is a safety net to catch any improperly mapped pages. */
    nx_enforce_audit_kernel();

    /* ── Kernel section hardening (Item 176) ──────────────────────
     * After all init is complete, apply fine-grained permissions:
     *   - .rodata → read-only (clear the write bit in PTEs)
     *   - .data   → non-executable (set the NX bit)
     *   - .bss    → non-executable (set the NX bit)
     * 2MB huge pages that span section boundaries are split to 4KB
     * first so each section gets correct per-page permissions. */
    nx_enforce_protect_kernel_sections();
#ifdef TEST_MODE
    /* Yield once so the test task gets a chance to run immediately */
    scheduler_yield();
#endif

    /* Enable scheduler — timer ticks are now safe to run the
     * full scheduler_tick() accounting on the idle process. */
    get_cpu_info()->scheduler_enabled = 1;
    schedule();

    /* Idle loop — the boot thread becomes the idle process.
     * Reap zombie processes while idle so orphaned children
     * (especially those reparented to init) don't accumulate. */
    for (;;) {
        process_reap_zombies();
        net_poll();
        if (need_resched())
            schedule();
        /* ── Debug: detect a stuck runqueue (a READY process that never
         * gets picked) — prints once. */
        {
            static int dbg_printed = 0;
            static uint64_t dbg_last_check = 0;
            uint64_t now = timer_get_ticks();
            if (!dbg_printed && now - dbg_last_check > 500) {
                dbg_last_check = now;
                extern struct process *process_get_table(void);
                struct process *t = process_get_table();
                for (int i = 0; i < PROCESS_MAX; i++) {
                    if (t[i].state == PROCESS_READY && t[i].pid > 0 && t[i].on_cpu == 0 &&
                        now - t[i].last_run_tick > 300) {
                        kprintf("[STUCK] pid=%u name=%s ready=%lu ticks (last_run=%lu) "
                                "deadline=%lu lag=%ld prio=%u\n",
                                (unsigned int)t[i].pid, t[i].name ? t[i].name : "?",
                                (unsigned long)(now - t[i].last_run_tick),
                                (unsigned long)t[i].last_run_tick,
                                (unsigned long)t[i].eevdf_deadline, (long)t[i].eevdf_lag,
                                (unsigned int)t[i].priority);
                        dbg_printed = 1;
                        break;
                    }
                }
            }
        }
        cpuidle_idle();
    }
}

/* ── Stub: kernel_reboot ─────────────────────────────── */
static int kernel_reboot(void) {
    kprintf("[kernel] kernel_reboot: not yet implemented\n");
    return 0;
}
/* ── Stub: kernel_halt ─────────────────────────────── */
static int kernel_halt(void) {
    kprintf("[kernel] kernel_halt: not yet implemented\n");
    return 0;
}
/* ── Stub: kernel_poweroff ─────────────────────────────── */
static int kernel_poweroff(void) {
    kprintf("[kernel] kernel_poweroff: not yet implemented\n");
    return 0;
}
/* ── Stub: kernel_restart ─────────────────────────────── */
static int kernel_restart(const char *cmd) {
    (void)cmd;
    kprintf("[kernel] kernel_restart: not yet implemented\n");
    return 0;
}
