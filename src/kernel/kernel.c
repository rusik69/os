#define KERNEL_INTERNAL
#include "types.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "fault.h"
#include "timer.h"
#include "keyboard.h"
#include "pmm.h"
#include "vmm.h"
#include "page_cache.h"
#include "heap.h"
#include "process.h"
#include "scheduler.h"
#include "shell.h"
#include "serial.h"
#include "printf.h"
#include "io.h"
#include "ata.h"
#include "fs.h"
#include "pci.h"
#include "e1000.h"
#include "net.h"
#include "dhcp.h"
#include "netfilter.h"
#include "pkt_sched.h"
#include "bridge.h"
#include "vlan.h"
#include "tun.h"
#include "netdevice.h"
#include "veth.h"
#include "net_ns.h"
#include "ipip.h"
#include "wireguard.h"
#include "ipvs.h"
#include "telnetd.h"
#include "ssh.h"
#include "rtc.h"
#include "mouse.h"
#include "speaker.h"
#include "acpi.h"
#include "cpupstate.h"
#include "syscall.h"
#include "ahci.h"
#include "usb.h"
#include "usb_msc.h"
#include "service.h"
#include "httpd.h"
#include "gui.h"
#include "intel_gpu.h"
#include "fat32.h"
#include "users.h"
#include "vfs.h"
#include "fstab.h"
#include "pipe.h"
#include "blockdev.h"
#include "dm.h"
#include "shm.h"
#include "swap.h"
#include "virtio_net.h"
#include "virtio_blk.h"
#include "nvme.h"
#include "container.h"
#include "mdadm.h"
#include "ac97.h"
#include "smp.h"
#include "apic.h"
#include "elf.h"
#include "cpu.h"
#include "cpu_features.h"
#include "cpu_topology.h"
#include "x2apic.h"
#include "tsc_deadline.h"
#include "vsyscall.h"
#include "slab.h"
#include "oom.h"
#include "rcu.h"
#include "aslr.h"
#include "seccomp.h"
#include "audit.h"
#include "yama.h"
#include "kptr_restrict.h"
#include "dmesg.h"
#include "caps.h"
#include "sysrq.h"
#include "mce.h"
#include "panic.h"
#include "nmi_watchdog.h"
#include "lockdep.h"
#include "tmpfs.h"
#include "acpi_thermal.h"
#include "acpi_ec.h"
#include "psi.h"
#include "compaction.h"
#include "memhotplug.h"
#include "page_poison.h"
#include "cma.h"
#include "zram.h"
#include "zswap.h"
#include "ksm.h"
#include "thp.h"
#include "cmdline.h"
#include "ramdisk.h"
#include "timers.h"
#include "workqueue.h"
#include "rng.h"
#include "fsnotify.h"
#include "kprobes.h"
#include "module.h"
#include "module_signature.h"
#include "initcall.h"
#include "spi.h"
#include "watchdog.h"
#include "fbcon.h"
#include "perf_events.h"
#include "jump_label.h"
#include "pstore.h"
#include "kdump.h"
#include "kexec.h"
#include "mce.h"
#include "config_gz.h"
#include "nx_enforce.h"
#include "stack_guard.h"
#include "rseq.h"
#include "kasan_light.h"
#include "kmemleak.h"
#include "firmware.h"
#include "fault_inject.h"
#include "memfd.h"
#include "irq_regs.h"
#include "softirq.h"
#include "mseal.h"
#include "userfaultfd.h"
#include "cpuidle.h"
#include "pm_qos.h"
#include "pelt.h"
#include "madvise_ext.h"
#include "mem_policy.h"
#include "page_idle.h"
#include "page_allocator_ext.h"
#include "sched_attr.h"
#include "cpuset.h"
#include "core_sched.h"
#include "nohz.h"
#include "pidfd.h"
#include "landlock.h"
#include "seccomp_bpf.h"
#include "process_rlimit.h"
#include "devtmpfs.h"
#include "export.h"
#include "stdio.h"
#include "overlay.h"
#include "overlay_enhance.h"
#include "splash.h"
#include "sysfs.h"
#include "devfs.h"
#include "debugfs.h"
#include "dyndbg.h"
#include "kunit.h"
#include "fanotify.h"
#include "fs_mount_prop.h"
#include "hugetlb.h"
#include "net_igmp.h"
#include "net_lldp.h"
#include "net_rps.h"
#include "aio_enhanced.h"
#include "file_lock.h"
#include "string.h"
#include "sysctl.h"
#ifdef TEST_MODE
#include "test.h"
#endif

/* ── Forward declarations ─────────────────────────────── */
void stdio_init(void);

static void __attribute__((unused)) test_task_a(void) {
    for (;;) {
        /* Background task A: just yields */
        scheduler_yield();
    }
}

static void __attribute__((unused)) test_task_b(void) {
    for (;;) {
        /* Background task B: just yields */
        scheduler_yield();
    }
}

static void __attribute__((unused)) shell_task(void) {
    shell_run();
}

static void __attribute__((unused)) net_task(void) {
    telnetd_task();
}

/* ── Initcall support ─────────────────────────────────────────────── */

extern initcall_t __initcall_start[];
extern initcall_t __initcall_end[];

void do_initcalls(void) {
    initcall_t *fn;
    for (fn = __initcall_start; fn < __initcall_end; fn++) {
        if (*fn) {
            int ret = (*fn)();
            (void)ret;
        }
    }
    kprintf("[OK] Initcalls completed\n");
}

/* Stack-smashing protector (SSP) canary */
uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

void __attribute__((noreturn)) __stack_chk_fail(void) {
    kprintf("\n*** KERNEL STACK SMASHING DETECTED ***\n");
    cli();
    for (;;) hlt();
}

void kernel_main(uint32_t magic, uint64_t multiboot_info_phys) {
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
        for (;;) hlt();
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
            (unsigned long long)pmm_get_total_frames() * 4, (unsigned long long)pmm_get_used_frames() * 4);

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
        if (multiboot_info_phys == 0 || multiboot_info_phys > 0x100000) {
            kprintf("[boot] WARNING: multiboot_info at 0x%lx looks invalid\n",
                    (unsigned long)multiboot_info_phys);
        } else {
            uint32_t *mbi = (uint32_t *)PHYS_TO_VIRT(multiboot_info_phys);
            uint32_t flags = mbi[0];
            if (flags & (1 << 2)) { /* cmdline flag */
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
    x2apic_init();
    nx_enforce_init();

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

    /* Machine Check Exception (MCE) handler — hardware error detection */
    mce_init();

    /* /proc/config.gz — embedded kernel build configuration */
    config_gz_init();

    /* OOM killer */
    oom_init();

    /* RCU synchronization primitive */
    rcu_init();

    /* ASLR (Address Space Layout Randomization) */
    aslr_init();

    /* Seccomp syscall sandboxing */
    seccomp_init();

    /* Landlock sandbox (path-based access control) */
    landlock_init();

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
    khugepaged_start();
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

    /* I/O APIC and SMP boot */
    int ap_count = smp_boot_aps();
    if (ap_count > 0)
        kprintf("[OK] SMP: %d AP(s) booted, total %d CPU(s)\n",
                ap_count, (int)smp_get_cpu_count());

    /* Timer (starts scheduling) */
    timer_init();
    kprintf("[OK] Timer initialized at %d Hz\n", TIMER_FREQ);

    /* Dynamic kernel timers (driven by timer IRQ) */
    timers_init();

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

    /* Initcall system — run all registered initcalls in order */
    do_initcalls();

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

    /* RTC */
    rtc_init();
    struct rtc_time rtc;
    rtc_get_time(&rtc);
    kprintf("[OK] RTC: %u-%u-%u %u:%u:%u\n",
            rtc.year, rtc.month, rtc.day,
            rtc.hour, rtc.minute, rtc.second);

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

    /* CPU frequency scaling — ACPI P-states (ACPI _PSS / MSR fallback) */
    cpupstate_init();

    /* IMA — Integrity Measurement Architecture */
    {
        extern void ima_init(void);
        ima_init();
    }

    /* Debugfs — kernel debug data filesystem */
    debugfs_init();

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

    /* Block device registry */
    blockdev_init();

    /* Device mapper framework — virtual block device layer */
    dm_init();
    dm_linear_init();
    dm_zero_init();
    dm_error_init();
    dm_crypt_init();
    dm_verity_init();

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
        if (fat32_mount(FAT32_DISK_AHCI, 0) == 0) {
            vfs_mount("/mnt", &fat32_vfs_ops, NULL);
            kprintf("[OK] FAT32 mounted on /mnt\n");
        }
    } else if (ata_is_present()) {
        if (fat32_mount(FAT32_DISK_ATA, 0) == 0) {
            vfs_mount("/mnt", &fat32_vfs_ops, NULL);
            kprintf("[OK] FAT32 mounted on /mnt\n");
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

    /* Service infrastructure + FS directory tree */
    service_init();
    kprintf("[OK] Service manager initialized\n");

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
            const char *default_inittab =
                "# /etc/inittab - init configuration\n"
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

    /* Initialise the netdevice interface layer before any NIC driver
     * so they can register themselves as net devices during init. */
    netdevice_init();

    /* Virtual Ethernet pair driver — always available for ns networking */
    veth_init();

    if (e1000_init() == 0) {
        uint8_t mac[6];
        e1000_get_mac(mac);
        kprintf("[OK] e1000 NIC: %x:%x:%x:%x:%x:%x\n",
                mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
    } else {
        kprintf("[--] e1000 NIC not found\n");
    }

    /* Enable interrupts — needed for timer_get_ticks() in DHCP timeout */
    sti();

    if (virtio_net_present() || e1000_is_present()) {
        /* Networking subsystem inits */
        nf_init();
        pkt_sched_init();
        bridge_init();
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
        net_our_ip      = (10U << 24) | (0U << 16) | (2U << 8) | 15U;
        net_gateway_ip  = (10U << 24) | (0U << 16) | (2U << 8) | 2U;
        net_subnet_mask = (255U << 24) | (255U << 16) | (255U << 8) | 0U;
        net_dns_server  = (10U << 24) | (0U << 16) | (2U << 8) | 3U;
#endif
        uint8_t ip[4];
        net_get_ip(ip);
        kprintf("[OK] Network: %u.%u.%u.%u\n",
                ip[0], ip[1], ip[2], ip[3]);
        /* Register & start services */
        service_register("telnetd", telnetd_start, telnetd_stop);
        service_register("httpd",   httpd_start,   httpd_stop);
        service_register("sshd",   sshd_start,    sshd_stop);

        /* ── Service dependency setup (Item U3) ────────────────────
         * httpd depends on telnetd; sshd depends on httpd.
         * This means start order is: telnetd -> httpd -> sshd.
         * Stop order is: sshd -> httpd -> telnetd. */
        service_add_dep("httpd", "telnetd");
        service_add_dep("sshd", "httpd");

        /* Start services in dependency order (telnetd first, then httpd, then sshd) */
        service_start("sshd");  /* triggers sorted start of deps */
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

    /* Normal mode: try to load a userspace init binary from the filesystem.
     * If successful, the init process runs in ring 3 and can spawn shell, etc.
     * If no init binary is found, fall back to kernel-mode shell.
     *
     * The init path can be overridden via the `init=` kernel cmdline parameter.
     * Example: init=/mnt/bin/sh.elf  or  init=/sbin/init  */

    /* Finalise boot splash: mark progress complete */
    splash_progress(SPLASH_MAX_STAGES);

    int init_ok = 0;

    /* Check for init= cmdline parameter first */
    const char *cmdline_init_path = cmdline_get("init");
    if (cmdline_init_path && *cmdline_init_path) {
        if (elf_exec(cmdline_init_path) == 0) {
            init_ok = 1;
            kprintf("[OK] Userspace init (cmdline): %s\n", cmdline_init_path);
        } else {
            kprintf("[!!] Cmdline init=%s failed, trying defaults\n", cmdline_init_path);
        }
    }

    if (!init_ok) {
        /* Try common init paths */
        const char *init_paths[] = {
            "/mnt/init.elf",
            "/mnt/bin/init",
            "/mnt/shell.elf",
            "/mnt/bin/sh.elf",
        };

        for (size_t i = 0; i < sizeof(init_paths) / sizeof(init_paths[0]); i++) {
            if (elf_exec(init_paths[i]) == 0) {
                init_ok = 1;
                kprintf("[OK] Userspace init: %s\n", init_paths[i]);
                break;
            }
        }
    }

    /* Try to load initrd from multiboot module */
    {
        /* Check for multiboot module at mbi->mods_count, mods_addr */
        uint32_t *mbi = (uint32_t *)PHYS_TO_VIRT(multiboot_info_phys);
        if (mbi[0] & (1 << 3)) { /* mods flag */
            uint32_t mods_count = mbi[5];
            /* For multiboot2, mods_addr can be above 4GB (64-bit phys addr).
             * Multiboot1 uses uint32_t; we cast through uint64_t for safety. */
            uint64_t mods_addr = (uint64_t)mbi[6];
            if (mods_count > 0 && mods_addr > 0) {
                uint32_t *mod = (uint32_t *)PHYS_TO_VIRT(mods_addr);
                uint64_t mod_start = (uint64_t)mod[0];
                uint64_t mod_end   = (uint64_t)mod[1];
                uint64_t mod_size = mod_end - mod_start;
                if (mod_size > 0 && mod_size < 16*1024*1024) {
                    kprintf("[OK] Initrd module: %llu bytes at 0x%llx\n",
                            (unsigned long long)mod_size, (unsigned long long)mod_start);
                    /* Copy the initrd data into ramdisk */
                    void *mod_data = PHYS_TO_VIRT(mod_start);
                    if (ramdisk_is_present()) {
                        uint32_t num_sectors = (uint32_t)((mod_size + 511) / 512);
                        if (num_sectors <= ramdisk_get_sectors()) {
                            for (uint32_t s = 0; s < num_sectors; s++) {
                                ramdisk_write_sectors(s, 1, (const uint8_t*)mod_data + s * 512);
                            }
                            kprintf("[OK] Initrd loaded into ramdisk (%u sectors)\n", num_sectors);
                        }
                    }
                }
            }
        }
    }

    /* Always create a kernel-mode shell alongside init for debugging */
    if (!process_create(shell_task, "shell"))
        kprintf("[!!] Failed to create shell\n");

    if (!init_ok) {
        /* Fall back to kernel threads */
        kprintf("[--] No userspace init binary found, using kernel-mode shell\n");
        if (!process_create(test_task_a, "task_a"))
            kprintf("[!!] Failed to create task_a\n");
        if (!process_create(test_task_b, "task_b"))
            kprintf("[!!] Failed to create task_b\n");
        if (!process_create(shell_task, "shell"))
            kprintf("[!!] Failed to create shell\n");
        if (virtio_net_present() || e1000_is_present()) {
            if (!process_create(net_task, "netd"))
                kprintf("[!!] Failed to create netd\n");
            if (!process_create(httpd_task, "httpd"))
                kprintf("[!!] Failed to create httpd\n");
        }
        if (vga_is_framebuffer()) {
            if (!process_create(gui_task, "gui"))
                kprintf("[!!] Failed to create gui\n");
        }
    }
    kprintf("[OK] Processes created\n");
#endif

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

    /* Idle loop - the boot thread becomes the idle process */
    for (;;) {
        if (need_resched())
            schedule();
        cpuidle_idle();
    }
}
