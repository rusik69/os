#include "types.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
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
#include "intel_gpu.h"
#include "fat32.h"
#include "users.h"
#include "vfs.h"
#include "pipe.h"
#include "blockdev.h"
#ifdef TEST_MODE
#include "test.h"
#endif

static void test_task_a(void) {
    for (;;) {
        /* Background task A: just yields */
        scheduler_yield();
    }
}

static void test_task_b(void) {
    for (;;) {
        /* Background task B: just yields */
        scheduler_yield();
    }
}

static void shell_task(void) {
    shell_run();
}

static void net_task(void) {
    telnetd_task();
}

void kernel_main(uint32_t magic, uint64_t multiboot_info_phys) {
    /* Initialize serial first for debug output */
    serial_init();

    /* Initialize VGA console first for output */
    vga_init();
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("Booting OS...\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* Verify multiboot */
    if (magic != 0x2BADB002) {
        kprintf("ERROR: Not booted by Multiboot (magic: 0x%x)\n", (uint64_t)magic);
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

    /* Physical memory manager */
    pmm_init(multiboot_info_phys);
    kprintf("[OK] PMM initialized: %u KB total, %u KB used\n",
            pmm_get_total_frames() * 4, pmm_get_used_frames() * 4);

    /* Virtual memory manager */
    vmm_init();
    kprintf("[OK] VMM initialized\n");
    if (vga_try_init_framebuffer(multiboot_info_phys) == 0)
        kprintf("[OK] Framebuffer console enabled\n");

    /* Kernel heap */
    heap_init();
    kprintf("[OK] Heap initialized\n");

    /* Process subsystem */
    process_init();
    kprintf("[OK] Process subsystem initialized\n");

    /* Scheduler */
    scheduler_init();
    kprintf("[OK] Scheduler initialized\n");

    /* Timer (starts scheduling) */
    timer_init();
    kprintf("[OK] Timer initialized at %d Hz\n", (uint64_t)TIMER_FREQ);

    /* Keyboard */
    keyboard_init();
    kprintf("[OK] Keyboard initialized\n");

    /* RTC */
    rtc_init();
    struct rtc_time rtc;
    rtc_get_time(&rtc);
    kprintf("[OK] RTC: %u-%u-%u %u:%u:%u\n",
            (uint64_t)rtc.year, (uint64_t)rtc.month, (uint64_t)rtc.day,
            (uint64_t)rtc.hour, (uint64_t)rtc.minute, (uint64_t)rtc.second);

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

    /* VFS */
    vfs_init();
    kprintf("[OK] VFS initialized\n");

    /* Pipes */
    pipe_init();
    kprintf("[OK] Pipes initialized\n");

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

    /* Filesystem */
    fs_init();
    kprintf("[OK] Filesystem initialized\n");

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

    /* FAT32 */
    fat32_mount(ahci_is_present() ? FAT32_DISK_AHCI : FAT32_DISK_ATA, 0);

    /* Multiuser */
    users_init();
    kprintf("[OK] Multiuser initialized\n");

    /* Network */
    if (e1000_init() == 0) {
        uint8_t mac[6];
        e1000_get_mac(mac);
        kprintf("[OK] e1000 NIC: %x:%x:%x:%x:%x:%x\n",
                (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
                (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5]);
        net_init();
        kprintf("[..] DHCP discovering...\n");
        net_dhcp_discover();
        uint8_t ip[4];
        net_get_ip(ip);
        kprintf("[OK] Network: %u.%u.%u.%u\n",
                (uint64_t)ip[0], (uint64_t)ip[1], (uint64_t)ip[2], (uint64_t)ip[3]);
        telnetd_init();
        kprintf("[OK] Telnet server on port 23\n");
    } else {
        kprintf("[--] No network device found\n");
    }

#ifdef TEST_MODE
    /* Test mode: run the test suite then shut down */
    process_create(test_run_all, "tests");
    kprintf("[OK] Test task created\n");
#else
    /* Normal mode: interactive shell + background tasks */
    process_create(test_task_a, "task_a");
    process_create(test_task_b, "task_b");
    process_create(shell_task, "shell");
    if (e1000_is_present())
        process_create(net_task, "netd");
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
