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
#include "telnetd.h"
#include "rtc.h"
#include "mouse.h"
#include "speaker.h"
#include "acpi.h"
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
#include "pipe.h"
#include "blockdev.h"
#include "shm.h"
#include "virtio_net.h"
#include "virtio_blk.h"
#include "ac97.h"
#include "smp.h"
#include "apic.h"
#include "elf.h"
#include "cpu.h"
#include "slab.h"
#ifdef TEST_MODE
#include "test.h"
#endif

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

    /* Page fault handler */
    fault_init();
    kprintf("[OK] Page fault handler registered\n");

    /* Physical memory manager */
    pmm_init(multiboot_info_phys);
    kprintf("[OK] PMM initialized: %llu KB total, %llu KB used\n",
            (unsigned long long)pmm_get_total_frames() * 4, (unsigned long long)pmm_get_used_frames() * 4);

    /* Virtual memory manager */
    vmm_init();
    kprintf("[OK] VMM initialized\n");

    /* Enable CPU security features: SMEP, SMAP, NXE, UMIP */
    cpu_security_init();

    /* Kernel heap (framebuffer may allocate from heap) */
    heap_init();
    kprintf("[OK] Heap initialized\n");

    /* Slab allocator (for fixed-size kernel objects) */
    slab_init();

    if (vga_try_init_framebuffer(multiboot_info_phys) == 0)
        kprintf("[OK] Framebuffer console enabled\n");
    else
        kprintf("[OK] VGA text console (QEMU window or serial terminal)\n");

    /* Process subsystem */
    process_init();
    kprintf("[OK] Process subsystem initialized\n");

    /* Per-CPU data for SMP — must be before scheduler_init (needs GS_BASE) */
    smp_init_bsp();
    kprintf("[OK] SMP per-CPU data initialized\n");

    /* Scheduler */
    scheduler_init();
    kprintf("[OK] Scheduler initialized\n");

    /* Local APIC (replaces PIC for interrupt delivery) */
    apic_init_local();
    kprintf("[OK] Local APIC initialized\n");

    /* Register IPI handlers for SMP coordination */
    ipi_init();
    kprintf("[OK] IPI handlers registered\n");

    /* I/O APIC and SMP boot */
    int ap_count = smp_boot_aps();
    if (ap_count > 0)
        kprintf("[OK] SMP: %d AP(s) booted, total %d CPU(s)\n",
                ap_count, (int)smp_get_cpu_count());

    /* Timer (starts scheduling) */
    timer_init();
    kprintf("[OK] Timer initialized at %d Hz\n", TIMER_FREQ);

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

    /* PS/2 Mouse */
    mouse_init();
    kprintf("[OK] Mouse initialized\n");

    /* PC Speaker */
    speaker_init();
    kprintf("[OK] Speaker initialized\n");

    /* ACPI */
    acpi_init();

    /* Syscall interface */
    syscall_init();
    kprintf("[OK] Syscall interface initialized\n");

    /* Production subsystems (socket, epoll, timers, mq) */
    production_subsystems_init();
    kprintf("[OK] Production subsystems initialized\n");

    /* VFS */
    vfs_init();
    kprintf("[OK] VFS initialized\n");

    /* Pipes */
    pipe_init();
    kprintf("[OK] Pipes initialized\n");

    /* Shared memory */
    shm_init();
    kprintf("[OK] Shared memory initialized\n");

    /* Block device registry */
    blockdev_init();

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

    /* Filesystem */
    fs_init();
    kprintf("[OK] Filesystem initialized\n");

    /* Service infrastructure + FS directory tree */
    service_init();
    kprintf("[OK] Service manager initialized\n");

    /* PCI bus */
    pci_init();
    kprintf("[OK] PCI initialized\n");

    /* Intel integrated GPU */
    if (intel_gpu_init() == 0)
        kprintf("[OK] Intel GPU initialized\n");
    else
        kprintf("[--] No Intel GPU found\n");

    /* USB */
    if (usb_init() == 0) {
        kprintf("[OK] USB initialized\n");
        if (usb_msc_init() == 0)
            kprintf("[OK] USB MSC device registered\n");
        else
            kprintf("[--] No USB MSC device\n");
    } else {
        kprintf("[--] No USB controllers\n");
    }

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

    if (ac97_init() == 0)
        kprintf("[OK] AC97 audio: initialized\n");
    else
        kprintf("[--] AC97 audio: not present\n");

    if (e1000_init() == 0) {
        uint8_t mac[6];
        e1000_get_mac(mac);
        kprintf("[OK] e1000 NIC: %x:%x:%x:%x:%x:%x\n",
                mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
    } else {
        kprintf("[--] e1000 NIC not found\n");
    }

    if (virtio_net_present() || e1000_is_present()) {
        net_init();
        kprintf("[..] DHCP discovering...\n");
        net_dhcp_discover();
        uint8_t ip[4];
        net_get_ip(ip);
        kprintf("[OK] Network: %u.%u.%u.%u\n",
                ip[0], ip[1], ip[2], ip[3]);
        /* Register & start services */
        service_register("telnetd", telnetd_start, telnetd_stop);
        service_register("httpd",   httpd_start,   httpd_stop);
        service_start("telnetd");
        service_start("httpd");
        kprintf("[OK] Services started\n");
    } else {
        kprintf("[--] No network device found\n");
    }

#ifdef TEST_MODE
    /* Test mode: run the test suite then shut down */
    if (!process_create(test_run_all, "tests"))
        kprintf("[!!] Failed to create test process\n");
    else
        kprintf("[OK] Test task created\n");
#else
    /* Normal mode: try to load a userspace init binary from the filesystem.
     * If successful, the init process runs in ring 3 and can spawn shell, etc.
     * If no init binary is found, fall back to kernel-mode shell. */
    int init_ok = 0;

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

    /* Enable interrupts */
    sti();
    kprintf("[OK] Interrupts enabled\n\n");

    /* Idle loop - the boot thread becomes the idle process */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
