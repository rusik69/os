#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "fs.h"
#include "vfs.h"
#include "ata.h"
#include "ahci.h"
#include "vga.h"
#include "timer.h"
#include "keyboard.h"
#include "printf.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "net.h"
#include "e1000.h"
#include "pci.h"
#include "usb.h"
#include "users.h"
#include "rtc.h"
#include "acpi.h"
#include "speaker.h"
#include "mouse.h"
#include "serial.h"
#include "pmm.h"
#include "io.h"
#include "elf.h"
#include "script.h"
#include "fat32.h"
#include "shell.h"
#include "cc.h"
#include "heap.h"
#include "gui_shell.h"

struct syscall_fs_stat_ex {
    uint32_t size;
    uint8_t  type;
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
};

struct syscall_process_info {
    uint32_t pid;
    uint32_t ppid;
    uint8_t state;
    uint8_t is_user;
    uint8_t is_background;
    char name[32];
};

/* I/O and Memory structs (Phase 3 Group 3a) - must match libc definitions */
struct mouse_state {
    int x;
    int y;
    uint8_t buttons;
};

struct pmm_stats {
    uint32_t total_pages;
    uint32_t used_pages;
    uint32_t free_pages;
};

struct syscall_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t is_framebuffer;
};

/* MSR numbers */
#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_SFMASK 0xC0000084

/* EFER bits */
#define EFER_SCE (1 << 0)  /* Syscall Enable */

extern void syscall_entry(void);

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

#define SYSCALL_USER_STR_MAX 4096

static int syscall_is_user_process(void) {
    struct process *p = process_get_current();
    return (p && p->is_user && p->pml4);
}

static int syscall_user_read_ok(uint64_t addr, uint64_t len) {
    struct process *p = process_get_current();
    if (!p || !p->pml4) return 0;
    return vmm_user_range_ok(p->pml4, addr, len, 0);
}

static int syscall_user_write_ok(uint64_t addr, uint64_t len) {
    struct process *p = process_get_current();
    if (!p || !p->pml4) return 0;
    return vmm_user_range_ok(p->pml4, addr, len, 1);
}

static int syscall_user_cstr_ok(uint64_t addr) {
    struct process *p = process_get_current();
    if (!p || !p->pml4) return 0;
    return vmm_user_string_ok(p->pml4, addr, SYSCALL_USER_STR_MAX);
}

static int syscall_validate_user_args(uint64_t num, uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4, uint64_t a5) {
    if (!syscall_is_user_process()) return 1;

    switch (num) {
        case SYS_READ:
            return syscall_user_write_ok(a2, a3);
        case SYS_WRITE:
            return syscall_user_read_ok(a2, a3);
        case SYS_OPEN:
        case SYS_MKDIR:
        case SYS_UNLINK:
        case SYS_FS_DELETE:
        case SYS_FS_LIST:
        case SYS_VFS_UNLINK:
        case SYS_VFS_READDIR:
        case SYS_NET_DNS:
        case SYS_ELF_EXEC:
        case SYS_SCRIPT_EXEC:
        case SYS_FAT_FILE_SIZE:
        case SYS_SHELL_HISTORY_ADD:
            return syscall_user_cstr_ok(a1);
        case SYS_STAT:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(uint32_t) * 2);
        case SYS_FS_CREATE:
        case SYS_FS_CHMOD:
            return syscall_user_cstr_ok(a1);
        case SYS_FS_CHOWN:
            return syscall_user_cstr_ok(a1);
        case SYS_FS_WRITE:
            return syscall_user_cstr_ok(a1) && syscall_user_read_ok(a2, a3);
        case SYS_FS_READ:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3) &&
                   syscall_user_write_ok(a4, sizeof(uint32_t));
        case SYS_FS_STAT:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(uint32_t) * 2);
        case SYS_FS_STAT_EX:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(struct syscall_fs_stat_ex));
        case SYS_FS_LIST_NAMES:
            if (!syscall_user_cstr_ok(a1)) return 0;
            if (a2 && !syscall_user_cstr_ok(a2)) return 0;
            if (a4 == 0) return 1;
            if (a4 > (1ULL << 20)) return 0;
            return syscall_user_write_ok(a3, a4 * FS_MAX_NAME);
        case SYS_VFS_READ:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3) &&
                   syscall_user_write_ok(a4, sizeof(uint32_t));
        case SYS_VFS_WRITE:
            return syscall_user_cstr_ok(a1) && syscall_user_read_ok(a2, a3);
        case SYS_VFS_STAT:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(struct vfs_stat));
        case SYS_WAITPID:
            return a2 ? syscall_user_write_ok(a2, sizeof(int)) : 1;
        case SYS_NET_GET_MAC:
            return syscall_user_write_ok(a1, 6);
        case SYS_NET_GET_IP:
            return syscall_user_write_ok(a1, 4);
        case SYS_NET_UDP_SEND:
            return syscall_user_read_ok(a4, a5);
        case SYS_NET_HTTP_GET: {
            uint64_t bufsize = (uint32_t)(a5 >> 32);
            return syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a3) &&
                   syscall_user_write_ok(a4, bufsize);
        }
        case SYS_PROC_LIST:
            if (a2 == 0) return 1;
            if (a2 > PROCESS_MAX) return 0;
            return syscall_user_write_ok(a1, a2 * sizeof(struct syscall_process_info));
        case SYS_USER_FIND:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(struct user_entry));
        case SYS_USER_ADD:
            return syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a3);
        case SYS_USER_PASSWD:
        case SYS_SESSION_LOGIN:
            return syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a2);
        case SYS_USER_DELETE:
            return syscall_user_cstr_ok(a1);
        case SYS_USERS_GET_BY_INDEX:
            return syscall_user_write_ok(a2, sizeof(struct user_entry));
        case SYS_RTC_GET_TIME:
            return syscall_user_write_ok(a1, sizeof(struct rtc_time));
        case SYS_MOUSE_GET_STATE:
            return syscall_user_write_ok(a1, sizeof(struct mouse_state));
        case SYS_SERIAL_READ:
            return syscall_user_write_ok(a1, a2);
        case SYS_SERIAL_WRITE:
            return syscall_user_read_ok(a1, a2);
        case SYS_PMM_GET_STATS:
            return syscall_user_write_ok(a1, sizeof(struct pmm_stats));
        case SYS_FAT_LIST_DIR:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3 * FAT32_MAX_NAME);
        case SYS_FAT_READ_FILE:
            return syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3);
        case SYS_SHELL_READ_LINE:
            return syscall_user_write_ok(a1, a2);
        case SYS_SHELL_VAR_SET:
        case SYS_CC_COMPILE:
            return syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a2);
        case SYS_SHELL_EXEC_CMD:
            if (!syscall_user_cstr_ok(a1)) return 0;
            return a2 ? syscall_user_cstr_ok(a2) : 1;
        case SYS_SHELL_TAB_COMPLETE:
            return syscall_user_write_ok(a1, 1) && syscall_user_write_ok(a2, sizeof(int));
        default:
            return 1;
    }
}

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ── Syscall handlers ─────────────────────────────────────────── */

static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    (void)fd; (void)buf_addr; (void)len;
    /* Placeholder: stdin not wired for user processes yet */
    return (uint64_t)-1;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    if (fd == 1 || fd == 2) {
        const char *s = (const char *)buf_addr;
        for (uint64_t i = 0; i < len; i++)
            vga_putchar(s[i]);
        return len;
    }
    return (uint64_t)-1;
}

static uint64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode) {
    (void)flags; (void)mode;
    const char *path = (const char *)path_addr;
    uint32_t size; uint8_t type;
    if (fs_stat(path, &size, &type) < 0) return (uint64_t)-1;
    /* Return a dummy fd (full fd table not implemented yet) */
    return 3;
}

static uint64_t sys_close(uint64_t fd) {
    (void)fd;
    return 0;
}

static uint64_t sys_exit(uint64_t code) {
    (void)code;
    struct process *p = process_get_current();
    /* If this is a user-mode process, clean up page tables */
    if (p && p->is_user && p->pml4) {
        vmm_switch_pml4(vmm_get_pml4()); /* switch back to kernel pages */
        vmm_destroy_user_pml4(p->pml4);
        p->pml4 = NULL;
    }
    process_exit();
    return 0; /* unreachable */
}

static uint64_t sys_getpid(void) {
    struct process *p = process_get_current();
    return p ? (uint64_t)p->pid : 0;
}

static uint64_t sys_kill(uint64_t pid, uint64_t sig) {
    (void)sig;
    struct process *p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)-1;
    p->state = PROCESS_ZOMBIE;
    return 0;
}

static uint64_t sys_brk(uint64_t addr) {
    /* Minimal stub — user-space heap management not yet implemented */
    (void)addr;
    return addr;
}

static uint64_t sys_stat(uint64_t path_addr, uint64_t out_addr) {
    const char *path = (const char *)path_addr;
    uint32_t *out = (uint32_t *)out_addr;
    uint32_t size; uint8_t type;
    if (fs_stat(path, &size, &type) < 0) return (uint64_t)-1;
    if (out) { out[0] = size; out[1] = type; }
    return 0;
}

static uint64_t sys_mkdir(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    return (uint64_t)fs_create(path, 2 /* FS_TYPE_DIR */);
}

static uint64_t sys_unlink(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    return (uint64_t)fs_delete(path);
}

static uint64_t sys_time(void) {
    return timer_get_ticks() / TIMER_FREQ;
}

static uint64_t sys_yield(void) {
    scheduler_yield();
    return 0;
}

static uint64_t sys_uptime(void) {
    return timer_get_ticks();
}

static uint64_t sys_fs_format(void) {
    return (uint64_t)fs_format();
}

static uint64_t sys_fs_create(uint64_t path_addr, uint64_t type) {
    return (uint64_t)fs_create((const char *)path_addr, (uint8_t)type);
}

static uint64_t sys_fs_write(uint64_t path_addr, uint64_t data_addr, uint64_t size) {
    return (uint64_t)fs_write_file((const char *)path_addr, (const void *)data_addr, (uint32_t)size);
}

static uint64_t sys_fs_read(uint64_t path_addr, uint64_t buf_addr, uint64_t max_size, uint64_t out_addr) {
    return (uint64_t)fs_read_file((const char *)path_addr, (void *)buf_addr,
                                  (uint32_t)max_size, (uint32_t *)out_addr);
}

static uint64_t sys_fs_delete(uint64_t path_addr) {
    return (uint64_t)fs_delete((const char *)path_addr);
}

static uint64_t sys_fs_list(uint64_t path_addr) {
    return (uint64_t)fs_list((const char *)path_addr);
}

static uint64_t sys_fs_stat(uint64_t path_addr, uint64_t out_addr) {
    uint32_t size = 0;
    uint8_t type = 0;
    int rc = fs_stat((const char *)path_addr, &size, &type);
    if (rc < 0) return (uint64_t)rc;
    if (out_addr) {
        uint32_t *out = (uint32_t *)out_addr;
        out[0] = size;
        out[1] = type;
    }
    return 0;
}

static uint64_t sys_fs_stat_ex(uint64_t path_addr, uint64_t out_addr) {
    struct syscall_fs_stat_ex *out = (struct syscall_fs_stat_ex *)out_addr;
    uint32_t size = 0;
    uint8_t type = 0;
    uint16_t uid = 0, gid = 0, mode = 0;
    int rc = fs_stat_ex((const char *)path_addr, &size, &type, &uid, &gid, &mode);
    if (rc < 0) return (uint64_t)rc;
    if (out) {
        out->size = size;
        out->type = type;
        out->uid = uid;
        out->gid = gid;
        out->mode = mode;
    }
    return 0;
}

static uint64_t sys_fs_chmod(uint64_t path_addr, uint64_t mode) {
    return (uint64_t)fs_chmod((const char *)path_addr, (uint16_t)mode);
}

static uint64_t sys_fs_chown(uint64_t path_addr, uint64_t uid, uint64_t gid) {
    return (uint64_t)fs_chown((const char *)path_addr, (uint16_t)uid, (uint16_t)gid);
}

static uint64_t sys_fs_get_usage(uint64_t out_addr) {
    uint32_t used_inodes = 0, total_inodes = 0, used_blocks = 0, data_start = 0;
    fs_get_usage(&used_inodes, &total_inodes, &used_blocks, &data_start);
    if (out_addr) {
        uint32_t *out = (uint32_t *)out_addr;
        out[0] = used_inodes;
        out[1] = total_inodes;
        out[2] = used_blocks;
        out[3] = data_start;
    }
    return 0;
}

static uint64_t sys_fs_list_names(uint64_t dir_addr, uint64_t prefix_addr,
                                  uint64_t names_addr, uint64_t max) {
    return (uint64_t)fs_list_names((const char *)dir_addr, (const char *)prefix_addr,
                                   (char (*)[FS_MAX_NAME])names_addr, (int)max);
}

static uint64_t sys_ata_present(void) {
    return (uint64_t)ata_is_present();
}

static uint64_t sys_ata_sectors(void) {
    return (uint64_t)ata_get_sectors();
}

static uint64_t sys_ahci_present(void) {
    return (uint64_t)ahci_is_present();
}

static uint64_t sys_ahci_sectors(void) {
    return (uint64_t)ahci_get_sectors();
}

static uint64_t sys_vfs_read(uint64_t path_addr, uint64_t buf_addr, uint64_t max, uint64_t out_addr) {
    return (uint64_t)vfs_read((const char *)path_addr, (void *)buf_addr,
                              (uint32_t)max, (uint32_t *)out_addr);
}

static uint64_t sys_vfs_write(uint64_t path_addr, uint64_t data_addr, uint64_t size) {
    return (uint64_t)vfs_write((const char *)path_addr, (const void *)data_addr, (uint32_t)size);
}

static uint64_t sys_vfs_stat(uint64_t path_addr, uint64_t st_addr) {
    return (uint64_t)vfs_stat((const char *)path_addr, (struct vfs_stat *)st_addr);
}

static uint64_t sys_vfs_create(uint64_t path_addr, uint64_t type) {
    return (uint64_t)vfs_create((const char *)path_addr, (uint8_t)type);
}

static uint64_t sys_vfs_unlink(uint64_t path_addr) {
    return (uint64_t)vfs_unlink((const char *)path_addr);
}

static uint64_t sys_vfs_readdir(uint64_t path_addr) {
    return (uint64_t)vfs_readdir((const char *)path_addr);
}

static uint64_t sys_waitpid(uint64_t pid, uint64_t status_addr) {
    return (uint64_t)process_waitpid((uint32_t)pid, (int *)status_addr);
}

static uint64_t sys_sleep_ticks(uint64_t ticks) {
    process_sleep_ticks(ticks);
    return 0;
}

static uint64_t sys_net_present(void) {
    return (uint64_t)e1000_is_present();
}

static uint64_t sys_net_get_mac(uint64_t mac_addr) {
    uint8_t *mac = (uint8_t *)mac_addr;
    if (!mac) return (uint64_t)-1;
    e1000_get_mac(mac);
    return 0;
}

static uint64_t sys_net_get_ip(uint64_t ip_addr) {
    uint8_t *ip = (uint8_t *)ip_addr;
    if (!ip) return (uint64_t)-1;
    net_get_ip(ip);
    return 0;
}

static uint64_t sys_net_get_gw(void) {
    return (uint64_t)net_get_gateway();
}

static uint64_t sys_net_get_mask(void) {
    return (uint64_t)net_get_mask();
}

static uint64_t sys_net_dns(uint64_t host_addr) {
    return (uint64_t)net_dns_resolve((const char *)host_addr);
}

static uint64_t sys_net_ping(uint64_t ip) {
    return (uint64_t)net_ping((uint32_t)ip);
}

static uint64_t sys_net_udp_send(uint64_t dst_ip, uint64_t src_port, uint64_t dst_port,
                                 uint64_t data_addr, uint64_t len) {
    net_udp_send((uint32_t)dst_ip, (uint16_t)src_port, (uint16_t)dst_port,
                 (const void *)data_addr, (uint16_t)len);
    return 0;
}

static uint64_t sys_net_http_get(uint64_t host_addr, uint64_t port, uint64_t path_addr,
                                 uint64_t buf_addr, uint64_t packed) {
    int bufsize = (int)(uint32_t)(packed >> 32);
    int follow = (int)(uint32_t)packed;
    return (uint64_t)net_http_get_ex((const char *)host_addr, (uint16_t)port,
                                     (const char *)path_addr, (char *)buf_addr,
                                     bufsize, follow);
}

static void arp_print_entry_sys(uint32_t ip, const uint8_t *mac) {
    kprintf("  %u.%u.%u.%u  ->  %x:%x:%x:%x:%x:%x\n",
            (uint64_t)((ip >> 24) & 0xFF), (uint64_t)((ip >> 16) & 0xFF),
            (uint64_t)((ip >> 8) & 0xFF), (uint64_t)(ip & 0xFF),
            (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
            (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5]);
}

static uint64_t sys_net_arp_list(void) {
    int n = net_arp_list(arp_print_entry_sys);
    return (uint64_t)n;
}

static uint64_t sys_proc_list(uint64_t out_addr, uint64_t max) {
    struct syscall_process_info *out = (struct syscall_process_info *)out_addr;
    struct process *table = process_get_table();
    int written = 0;
    int lim = (int)max;
    if (lim < 0) lim = 0;
    for (int i = 0; i < PROCESS_MAX && written < lim; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        out[written].pid = table[i].pid;
        out[written].ppid = table[i].parent_pid;
        out[written].state = (uint8_t)table[i].state;
        out[written].is_user = (uint8_t)(table[i].is_user ? 1 : 0);
        out[written].is_background = (uint8_t)(table[i].is_background ? 1 : 0);
        if (table[i].name) {
            strncpy(out[written].name, table[i].name, sizeof(out[written].name) - 1);
            out[written].name[sizeof(out[written].name) - 1] = '\0';
        } else {
            out[written].name[0] = '\0';
        }
        written++;
    }
    return (uint64_t)written;
}

static uint64_t sys_pci_list(void) {
    pci_list();
    return 0;
}

static uint64_t sys_usb_list(void) {
    int n = usb_get_device_count();
    kprintf("USB devices: %d\n", (uint64_t)n);
    for (int i = 0; i < n; i++) {
        struct usb_device *dev = usb_get_device(i);
        if (!dev) continue;
        const char *spd = dev->speed == 2 ? "High" :
                          dev->speed == 1 ? "Low"  : "Full";
        kprintf("  Bus %03d Device %03d: %s-speed class=%02x\n",
                (uint64_t)1, (uint64_t)dev->addr,
                spd, (uint64_t)dev->class_code);
    }
    if (n == 0) kprintf("  (no devices connected)\n");
    return (uint64_t)n;
}

static uint64_t sys_hwinfo_print(void) {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];

    kprintf("=== Hardware Information ===\n");

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t *)&vendor[0] = ebx;
    *(uint32_t *)&vendor[4] = edx;
    *(uint32_t *)&vendor[8] = ecx;
    vendor[12] = 0;
    kprintf("CPU vendor: %s\n", vendor);

    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    kprintf("CPU family/model/stepping: %u/%u/%u\n",
            (uint64_t)((eax >> 8) & 0xF), (uint64_t)((eax >> 4) & 0xF), (uint64_t)(eax & 0xF));

    kprintf("PCI devices:\n");
    pci_list();
    return 0;
}

/* ── User/Session syscall handlers (Phase 3 Group 1) ─────── */

static uint64_t sys_user_find(uint64_t name_addr, uint64_t out_addr) {
    const char *username = (const char *)name_addr;
    struct user_entry *out = (struct user_entry *)out_addr;
    if (!username || !out) return (uint64_t)-1;
    return (uint64_t)user_find(username, out);
}

static uint64_t sys_user_add(uint64_t name_addr, uint64_t uid, uint64_t pass_addr) {
    struct user_session *sess = session_get();
    if (!sess || !sess->logged_in || sess->uid != 0) return (uint64_t)-2;

    const char *username = (const char *)name_addr;
    const char *password = (const char *)pass_addr;
    if (!username || !password) return (uint64_t)-1;
    return (uint64_t)user_add(username, (uint32_t)uid, password);
}

static uint64_t sys_user_delete(uint64_t name_addr) {
    struct user_session *sess = session_get();
    if (!sess || !sess->logged_in || sess->uid != 0) return (uint64_t)-2;

    const char *username = (const char *)name_addr;
    if (!username) return (uint64_t)-1;
    return (uint64_t)user_delete(username);
}

static uint64_t sys_user_passwd(uint64_t name_addr, uint64_t pass_addr) {
    struct user_session *sess = session_get();
    if (!sess || !sess->logged_in) return (uint64_t)-2;

    const char *username = (const char *)name_addr;
    const char *new_pass = (const char *)pass_addr;
    if (!username || !new_pass) return (uint64_t)-1;

    /* Linux-like: root can change any password; regular users only their own. */
    if (sess->uid != 0 && strcmp(username, sess->username) != 0) return (uint64_t)-2;

    return (uint64_t)user_passwd(username, new_pass);
}

static uint64_t sys_session_login(uint64_t name_addr, uint64_t pass_addr) {
    const char *username = (const char *)name_addr;
    const char *password = (const char *)pass_addr;
    if (!username || !password) return (uint64_t)-1;
    return (uint64_t)session_login(username, password);
}

static uint64_t sys_session_logout(void) {
    session_logout();
    return 0;
}

static uint64_t sys_session_get(void) {
    if (syscall_is_user_process()) return (uint64_t)-1;
    struct user_session *s = session_get();
    return (uint64_t)(uintptr_t)s;
}

static uint64_t sys_users_count(uint64_t mode) {
    if (mode == 0) {
        return (uint64_t)users_count();
    } else if (mode == 1) {
        if (syscall_is_user_process()) return (uint64_t)-1;
        /* Return pointer to kernel's user table for direct access */
        return (uint64_t)(uintptr_t)users_get_table();
    }
    return (uint64_t)-1;
}

static uint64_t sys_users_get_by_index(uint64_t idx, uint64_t out_addr) {
    struct user_entry *out = (struct user_entry *)out_addr;
    struct user_entry *tbl = users_get_table();
    int max = users_count();
    if (!out || (int)idx < 0 || (int)idx >= max) return (uint64_t)-1;
    *out = tbl[(int)idx];
    return 0;
}

static uint64_t sys_proc_set_cap_profile(uint64_t pid, uint64_t profile) {
    struct user_session *sess = session_get();
    if (!sess || !sess->logged_in || sess->uid != 0) return (uint64_t)-1;

    struct process *target = process_get_by_pid((uint32_t)pid);
    if (!target || target->state == PROCESS_UNUSED) return (uint64_t)-2;
    if (!target->is_user) return (uint64_t)-3;

    if (process_set_cap_profile(target, (enum process_cap_profile)profile) < 0)
        return (uint64_t)-4;

    return 0;
}

/* Hardware/Audio syscall handlers (Phase 3 Group 2) */

static uint64_t sys_speaker_beep(uint64_t frequency, uint64_t duration_ms) {
    speaker_beep((uint32_t)frequency, (uint32_t)duration_ms);
    return 0;
}

static uint64_t sys_rtc_get_time(uint64_t out_addr) {
    struct rtc_time *out = (struct rtc_time *)out_addr;
    if (!out) return (uint64_t)-1;
    rtc_get_time(out);
    return 0;
}

static uint64_t sys_acpi_shutdown(void) {
    acpi_shutdown();
    return 0;
}

/* I/O and Memory syscall handlers (Phase 3 Group 3a) */

static uint64_t sys_mouse_get_state(uint64_t out_addr) {
    struct mouse_state *out = (struct mouse_state *)out_addr;
    if (!out) return (uint64_t)-1;
    mouse_get_pos((int *)&out->x, (int *)&out->y);
    out->buttons = mouse_get_buttons();
    return 0;
}

static uint64_t sys_serial_read(uint64_t buf_addr, uint64_t max) {
    uint8_t *buf = (uint8_t *)buf_addr;
    if (!buf || max <= 0) return (uint64_t)-1;
    int n_read = 0;
    while (n_read < (int)max && serial_readable()) {
        buf[n_read++] = serial_getchar();
    }
    return (uint64_t)n_read;
}

static uint64_t sys_serial_write(uint64_t buf_addr, uint64_t len) {
    const char *buf = (const char *)buf_addr;
    if (!buf || len == 0) return (uint64_t)0;
    serial_write(buf);
    return (uint64_t)len;
}

static uint64_t sys_cmos_read_byte(uint64_t addr) {
    uint8_t reg = (uint8_t)addr;
    outb(0x70, reg & 0x7F);  /* mask NMI-disable bit */
    return (uint64_t)inb(0x71);
}

static uint64_t sys_pmm_get_stats(uint64_t out_addr) {
    struct pmm_stats *out = (struct pmm_stats *)out_addr;
    if (!out) return (uint64_t)-1;
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    out->total_pages = (uint32_t)total;
    out->used_pages = (uint32_t)used;
    out->free_pages = (uint32_t)(total - used);
    return 0;
}

/* Specialized syscall handlers (Phase 3 Group 3b) */

static uint64_t sys_elf_exec(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)-1;
    return (uint64_t)elf_exec(path);
}

static uint64_t sys_script_exec(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)-1;
    return (uint64_t)script_exec(path);
}

static uint64_t sys_fat_mount(uint64_t disk, uint64_t part_lba) {
    return (uint64_t)fat32_mount((fat32_disk_t)disk, (uint32_t)part_lba);
}

static uint64_t sys_fat_is_mounted(void) {
    return (uint64_t)fat32_is_mounted();
}

static uint64_t sys_fat_list_dir(uint64_t path_addr, uint64_t names_addr, uint64_t max) {
    return (uint64_t)fat32_list_dir((const char *)path_addr,
                                    (char (*)[FAT32_MAX_NAME])names_addr,
                                    (int)max);
}

static uint64_t sys_fat_read_file(uint64_t path_addr, uint64_t buf_addr, uint64_t max_size) {
    return (uint64_t)fat32_read_file((const char *)path_addr, (void *)buf_addr, (uint32_t)max_size);
}

static uint64_t sys_fat_file_size(uint64_t path_addr) {
    return (uint64_t)fat32_file_size((const char *)path_addr);
}

static uint64_t sys_shell_history_show(void) {
    shell_history_show_entries();
    return 0;
}

static uint64_t sys_shell_read_line(uint64_t buf_addr, uint64_t max) {
    if (!buf_addr || max == 0) return (uint64_t)-1;
    shell_read_line((char *)buf_addr, (int)max);
    return 0;
}

static uint64_t sys_shell_var_set(uint64_t name_addr, uint64_t value_addr) {
    const char *name = (const char *)name_addr;
    const char *value = (const char *)value_addr;
    if (!name || !value) return (uint64_t)-1;
    shell_var_set(name, value);
    return 0;
}

static uint64_t sys_shell_exec_cmd(uint64_t cmd_addr, uint64_t args_addr) {
    const char *cmd = (const char *)cmd_addr;
    const char *args = (const char *)args_addr;
    if (!cmd) return (uint64_t)-1;
    shell_exec_cmd(cmd, args);
    return 0;
}

static uint64_t sys_vga_set_color(uint64_t fg, uint64_t bg) {
    vga_set_color((uint8_t)fg, (uint8_t)bg);
    return 0;
}

static uint64_t sys_vga_get_fb_info(uint64_t out_addr) {
    struct syscall_fb_info *out = (struct syscall_fb_info *)out_addr;
    if (!out) return (uint64_t)-1;
    out->is_framebuffer = (uint8_t)(vga_is_framebuffer() ? 1 : 0);
    if (out->is_framebuffer) {
        vga_get_framebuffer_info(&out->width, &out->height, &out->pitch, &out->bpp);
    } else {
        out->width = 0;
        out->height = 0;
        out->pitch = 0;
        out->bpp = 0;
    }
    return 0;
}

static uint64_t sys_cc_compile(uint64_t inpath_addr, uint64_t outpath_addr) {
    const char *inpath = (const char *)inpath_addr;
    const char *outpath = (const char *)outpath_addr;
    if (!inpath || !outpath) return (uint64_t)-1;

    CompilerState *cc = (CompilerState *)kmalloc(sizeof(CompilerState));
    if (!cc) return (uint64_t)-1;
    memset(cc, 0, sizeof(CompilerState));

    uint32_t read_sz = 0;
    int r = vfs_read(inpath, cc->src, CC_SRC_MAX - 1, &read_sz);
    if (r < 0 || read_sz == 0) {
        kfree(cc);
        return (uint64_t)-2;
    }
    cc->src[read_sz] = '\0';
    cc->src_len = read_sz;

    cc_lex(cc);
    if (cc->error) {
        kfree(cc);
        return (uint64_t)-3;
    }

    cc_parse(cc);
    if (cc->error) {
        kfree(cc);
        return (uint64_t)-4;
    }

    if (cc_write_elf(cc, outpath) < 0) {
        kfree(cc);
        return (uint64_t)-5;
    }

    kfree(cc);
    return 0;
}

static uint64_t sys_keyboard_getchar(void) {
    return (uint64_t)(uint8_t)keyboard_getchar();
}

static uint64_t sys_shell_history_add(uint64_t cmd_addr) {
    const char *cmd_line = (const char *)cmd_addr;
    if (!cmd_line) return (uint64_t)-1;
    shell_history_add(cmd_line);
    return 0;
}

static uint64_t sys_shell_history_count(void) {
    return (uint64_t)shell_history_count();
}

static uint64_t sys_shell_history_entry(uint64_t idx) {
    if (syscall_is_user_process()) return (uint64_t)-1;
    return (uint64_t)(uintptr_t)shell_history_entry((int)idx);
}

static uint64_t sys_shell_tab_complete(uint64_t buf_addr, uint64_t len_addr, uint64_t session_addr) {
    char *buf = (char *)buf_addr;
    int *len = (int *)len_addr;
    void *session = (void *)session_addr;
    if (!buf || !len) return (uint64_t)-1;
    shell_tab_complete_telnet(buf, len, session);
    return 0;
}

static uint64_t sys_vga_put_entry_at(uint64_t ch, uint64_t color, uint64_t row, uint64_t col) {
    vga_put_entry_at((char)(uint8_t)ch, (uint8_t)color, (uint16_t)row, (uint16_t)col);
    return 0;
}

static uint64_t sys_vga_set_cursor(uint64_t row, uint64_t col) {
    vga_set_cursor((uint16_t)row, (uint16_t)col);
    return 0;
}

static uint64_t sys_vga_clear(void) {
    vga_clear();
    return 0;
}

static uint64_t sys_gui_shell_run(void) {
    gui_shell_run();
    return 0;
}

/* ── Dispatch table ───────────────────────────────────────────── */

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    if (syscall_is_user_process()) {
        struct process *p = process_get_current();
        if (!p || !process_caps_has(p, (uint32_t)num)) return (uint64_t)-1;
    }

    if (!syscall_validate_user_args(num, a1, a2, a3, a4, a5)) {
        return (uint64_t)-1;
    }

    switch (num) {
        case SYS_READ:   return sys_read(a1, a2, a3);
        case SYS_WRITE:  return sys_write(a1, a2, a3);
        case SYS_OPEN:   return sys_open(a1, a2, a3);
        case SYS_CLOSE:  return sys_close(a1);
        case SYS_EXIT:   return sys_exit(a1);
        case SYS_GETPID: return sys_getpid();
        case SYS_KILL:   return sys_kill(a1, a2);
        case SYS_BRK:    return sys_brk(a1);
        case SYS_STAT:   return sys_stat(a1, a2);
        case SYS_MKDIR:  return sys_mkdir(a1);
        case SYS_UNLINK: return sys_unlink(a1);
        case SYS_TIME:   return sys_time();
        case SYS_YIELD:  return sys_yield();
        case SYS_UPTIME: return sys_uptime();
        case SYS_FS_FORMAT:     return sys_fs_format();
        case SYS_FS_CREATE:     return sys_fs_create(a1, a2);
        case SYS_FS_WRITE:      return sys_fs_write(a1, a2, a3);
        case SYS_FS_READ:       return sys_fs_read(a1, a2, a3, a4);
        case SYS_FS_DELETE:     return sys_fs_delete(a1);
        case SYS_FS_LIST:       return sys_fs_list(a1);
        case SYS_FS_STAT:       return sys_fs_stat(a1, a2);
        case SYS_FS_STAT_EX:    return sys_fs_stat_ex(a1, a2);
        case SYS_FS_CHMOD:      return sys_fs_chmod(a1, a2);
        case SYS_FS_CHOWN:      return sys_fs_chown(a1, a2, a3);
        case SYS_FS_GET_USAGE:  return sys_fs_get_usage(a1);
        case SYS_FS_LIST_NAMES: return sys_fs_list_names(a1, a2, a3, a4);
        case SYS_ATA_PRESENT:   return sys_ata_present();
        case SYS_ATA_SECTORS:   return sys_ata_sectors();
        case SYS_AHCI_PRESENT:  return sys_ahci_present();
        case SYS_AHCI_SECTORS:  return sys_ahci_sectors();
        case SYS_VFS_READ:      return sys_vfs_read(a1, a2, a3, a4);
        case SYS_VFS_WRITE:     return sys_vfs_write(a1, a2, a3);
        case SYS_VFS_STAT:      return sys_vfs_stat(a1, a2);
        case SYS_VFS_CREATE:    return sys_vfs_create(a1, a2);
        case SYS_VFS_UNLINK:    return sys_vfs_unlink(a1);
        case SYS_VFS_READDIR:   return sys_vfs_readdir(a1);
        case SYS_WAITPID:       return sys_waitpid(a1, a2);
        case SYS_SLEEP_TICKS:   return sys_sleep_ticks(a1);
        case SYS_NET_PRESENT:   return sys_net_present();
        case SYS_NET_GET_MAC:   return sys_net_get_mac(a1);
        case SYS_NET_GET_IP:    return sys_net_get_ip(a1);
        case SYS_NET_GET_GW:    return sys_net_get_gw();
        case SYS_NET_GET_MASK:  return sys_net_get_mask();
        case SYS_NET_DNS:       return sys_net_dns(a1);
        case SYS_NET_PING:      return sys_net_ping(a1);
        case SYS_NET_UDP_SEND:  return sys_net_udp_send(a1, a2, a3, a4, a5);
        case SYS_NET_HTTP_GET:  return sys_net_http_get(a1, a2, a3, a4, a5);
        case SYS_NET_ARP_LIST:  return sys_net_arp_list();
        case SYS_PROC_LIST:     return sys_proc_list(a1, a2);
        case SYS_PCI_LIST:      return sys_pci_list();
        case SYS_USB_LIST:      return sys_usb_list();
        case SYS_HWINFO_PRINT:  return sys_hwinfo_print();
        case SYS_USER_FIND:     return sys_user_find(a1, a2);
        case SYS_USER_ADD:      return sys_user_add(a1, a2, a3);
        case SYS_USER_DELETE:   return sys_user_delete(a1);
        case SYS_USER_PASSWD:   return sys_user_passwd(a1, a2);
        case SYS_SESSION_LOGIN: return sys_session_login(a1, a2);
        case SYS_SESSION_LOGOUT: return sys_session_logout();
        case SYS_SESSION_GET:   return sys_session_get();
        case SYS_USERS_COUNT:   return sys_users_count(a1);
        case SYS_USERS_GET_BY_INDEX: return sys_users_get_by_index(a1, a2);
        case SYS_PROC_SET_CAP_PROFILE: return sys_proc_set_cap_profile(a1, a2);
        case SYS_SPEAKER_BEEP:  return sys_speaker_beep(a1, a2);
        case SYS_RTC_GET_TIME:  return sys_rtc_get_time(a1);
        case SYS_ACPI_SHUTDOWN: return sys_acpi_shutdown();
        case SYS_MOUSE_GET_STATE: return sys_mouse_get_state(a1);
        case SYS_SERIAL_READ:   return sys_serial_read(a1, a2);
        case SYS_SERIAL_WRITE:  return sys_serial_write(a1, a2);
        case SYS_CMOS_READ_BYTE: return sys_cmos_read_byte(a1);
        case SYS_PMM_GET_STATS: return sys_pmm_get_stats(a1);
        case SYS_ELF_EXEC:      return sys_elf_exec(a1);
        case SYS_SCRIPT_EXEC:   return sys_script_exec(a1);
        case SYS_FAT_MOUNT:     return sys_fat_mount(a1, a2);
        case SYS_FAT_IS_MOUNTED: return sys_fat_is_mounted();
        case SYS_FAT_LIST_DIR:  return sys_fat_list_dir(a1, a2, a3);
        case SYS_FAT_READ_FILE: return sys_fat_read_file(a1, a2, a3);
        case SYS_FAT_FILE_SIZE: return sys_fat_file_size(a1);
        case SYS_SHELL_HISTORY_SHOW: return sys_shell_history_show();
        case SYS_SHELL_READ_LINE: return sys_shell_read_line(a1, a2);
        case SYS_SHELL_VAR_SET: return sys_shell_var_set(a1, a2);
        case SYS_SHELL_EXEC_CMD: return sys_shell_exec_cmd(a1, a2);
        case SYS_VGA_SET_COLOR: return sys_vga_set_color(a1, a2);
        case SYS_VGA_GET_FB_INFO: return sys_vga_get_fb_info(a1);
        case SYS_CC_COMPILE: return sys_cc_compile(a1, a2);
        case SYS_KEYBOARD_GETCHAR: return sys_keyboard_getchar();
        case SYS_SHELL_HISTORY_ADD: return sys_shell_history_add(a1);
        case SYS_SHELL_HISTORY_COUNT: return sys_shell_history_count();
        case SYS_SHELL_HISTORY_ENTRY: return sys_shell_history_entry(a1);
        case SYS_SHELL_TAB_COMPLETE: return sys_shell_tab_complete(a1, a2, a3);
        case SYS_VGA_PUT_ENTRY_AT: return sys_vga_put_entry_at(a1, a2, a3, a4);
        case SYS_VGA_SET_CURSOR: return sys_vga_set_cursor(a1, a2);
        case SYS_VGA_CLEAR: return sys_vga_clear();
        case SYS_GUI_SHELL_RUN: return sys_gui_shell_run();
        default:         return (uint64_t)-1;
    }
}

/* ── Init ─────────────────────────────────────────────────────── */

void syscall_init(void) {
    /* Enable SCE bit in EFER */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* STAR: bits 47:32 = kernel CS (0x08), bits 63:48 = user CS-8 (0x18-8=0x10
     * The CPU loads CS=STAR[47:32] and SS=STAR[47:32]+8 on syscall
     * On sysret:   CS=STAR[63:48]+16, SS=STAR[63:48]+8
     * We want kernel CS=0x08, user CS=0x18 (ring-3 code at GDT index 3)
     * So STAR[63:48] = 0x10 → sysret CS = 0x10+16 = 0x1B (with RPL3 added by CPU)
     */
    uint64_t star = ((uint64_t)0x0008 << 32) | ((uint64_t)0x0010 << 48);
    wrmsr(MSR_STAR, star);

    /* LSTAR: syscall entry point */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* SFMASK: mask IF (bit 9) during syscall execution */
    wrmsr(MSR_SFMASK, (1 << 9));
}
