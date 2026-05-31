#define KERNEL_INTERNAL
#include "syscall.h"
#include "process.h"
#include "scheduler.h"
#include "signal.h"
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
#include "telnetd.h"
#include "fat32.h"
#include "shell.h"
#include "cc.h"
#include "heap.h"
#include "smp.h"
#include "pipe.h"
#include "users.h"
#include "gui_shell.h"
#include "shm.h"
#include "ac97.h"
#include "doom.h"

/* ── Open file descriptor table (for lseek support) ────────────── */

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
    uint32_t pgid;
    uint32_t sid;
    uint8_t state;
    uint8_t is_user;
    uint8_t is_background;
    uint8_t is_suspended;
    uint8_t priority;
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

/* Local TLB invalidation */
static inline void local_invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* EFER bits */
#define EFER_SCE (1 << 0)  /* Syscall Enable */

extern void syscall_entry(void);
extern uint64_t syscall_kernel_rsp;
extern uint64_t syscall_user_rip;
extern uint64_t syscall_user_rflags;

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
        case SYS_NET_TCP_SEND_CONN:
            return syscall_user_read_ok(a2, a3);
        case SYS_NET_TCP_RECV_CONN:
            return syscall_user_write_ok(a2, a3);
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
        case SYS_FAT_WRITE_FILE:
            return syscall_user_cstr_ok(a1) && syscall_user_read_ok(a2, a3);
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
        case SYS_FREE:
            /* Pointer could be any previously-allocated address; no range check needed */
            return 1;
        case SYS_REALLOC:
            /* old ptr is arbitrary; just validate that the new result ptr location is writable */
            return 1;
        case SYS_CHDIR:
        case SYS_TRUNCATE:
            return syscall_user_cstr_ok(a1);
        case SYS_GETCWD:
            if (a2 == 0) return 0;
            return syscall_user_write_ok(a1, a2);
        case SYS_FD_READ:
            return syscall_user_write_ok(a2, a3);
        case SYS_FD_WRITE:
            return syscall_user_read_ok(a2, a3);
        case SYS_RAW_SEND:
            if (a2 == 0 || a2 > 1514) return 0;
            return syscall_user_read_ok(a1, a2);
        default:
            return 1;
    }
}

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* ── Syscall handlers ─────────────────────────────────────────── */

/* Get per-process FD table entry */
static struct process_fd *sys_get_fd(int i) {
    struct process *p = process_get_current();
    if (!p || i < 0 || i >= PROCESS_FD_MAX) return NULL;
    return &p->fd_table[i];
}

static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    if (fd == 0) return 0; /* stdin EOF until telnet fd wiring */
    if (fd >= 3) {
        int i = (int)fd - 3;
        struct process_fd *pfd = sys_get_fd(i);
        if (!pfd || !pfd->used) return (uint64_t)-1;
        struct vfs_stat st;
        if (vfs_stat(pfd->path, &st) < 0) return (uint64_t)-1;
        uint32_t fsize = st.size;
        if (pfd->offset >= fsize) return 0;
        uint32_t avail = fsize - pfd->offset;
        uint32_t to_read = (uint32_t)len < avail ? (uint32_t)len : avail;
        uint32_t need_end = pfd->offset + to_read;
        if (need_end > fsize) need_end = fsize;
        uint8_t *tmp = kmalloc(need_end);
        if (!tmp) return (uint64_t)-1;
        uint32_t nread = 0;
        vfs_read(pfd->path, tmp, need_end, &nread);
        memcpy((void *)(uintptr_t)buf_addr, tmp + pfd->offset, to_read);
        kfree(tmp);
        pfd->offset += to_read;
        return (uint64_t)to_read;
    }
    return (uint64_t)-1;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    if (fd == 1 || fd == 2) {
        const char *s = (const char *)buf_addr;
        for (uint64_t i = 0; i < len; i++) {
            vga_putchar(s[i]);
            serial_putchar(s[i]);
        }
        return len;
    }
    return (uint64_t)-1;
}

static uint64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode) {
    (void)flags; (void)mode;
    const char *path = (const char *)path_addr;
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0) return (uint64_t)-1;
    /* Allocate fd slot in current process's table */
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-1;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fd_table[i].used) {
            strncpy(p->fd_table[i].path, path, 63);
            p->fd_table[i].path[63] = '\0';
            p->fd_table[i].offset = 0;
            p->fd_table[i].used = 1;
            return (uint64_t)(i + 3);
        }
    }
    return (uint64_t)-1;
}

static uint64_t sys_close(uint64_t fd) {
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (pfd) pfd->used = 0;
    return 0;
}

static uint64_t sys_exit(uint64_t code) {
    struct process *p = process_get_current();
    /* If this is a user-mode process, clean up page tables */
    if (p && p->is_user && p->pml4) {
        vmm_switch_pml4(vmm_get_pml4()); /* switch back to kernel pages */
        vmm_destroy_user_pml4(p->pml4);
        p->pml4 = NULL;
    }
    process_exit_code((int)code);
    return 0; /* unreachable */
}

static uint64_t sys_getpid(void) {
    struct process *p = process_get_current();
    return p ? (uint64_t)p->pid : 0;
}

static uint64_t sys_kill(uint64_t pid, uint64_t sig) {
    return (uint64_t)signal_send((uint32_t)pid, (int)sig);
}

static uint64_t sys_brk(uint64_t addr) {
    /* Minimal stub — user-space heap management not yet implemented */
    (void)addr;
    return addr;
}

/* ── Signal registration (SYS_SIGNAL=213) ──────────────────────── */
static uint64_t sys_signal(uint64_t signum, uint64_t handler_addr) {
    if (syscall_is_user_process() &&
        handler_addr != (uint64_t)SIG_DFL && handler_addr != (uint64_t)SIG_IGN)
        return (uint64_t)-1;
    signal_register((int)signum, (signal_handler_t)(uintptr_t)handler_addr);
    return 0;
}

/* ── File seek / truncate (SYS_LSEEK=214, SYS_TRUNCATE=215) ───── */
static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used) return (uint64_t)-1;
    struct vfs_stat st; uint32_t fsz = 0;
    if (vfs_stat(pfd->path, &st) == 0) fsz = st.size;
    uint32_t new_off;
    switch (whence) {
        case 0: new_off = (uint32_t)offset; break;
        case 1: new_off = pfd->offset + (uint32_t)offset; break;
        case 2: new_off = fsz + (uint32_t)offset; break;
        default: return (uint64_t)-1;
    }
    pfd->offset = new_off;
    return (uint64_t)new_off;
}

static uint64_t sys_truncate(uint64_t path_addr, uint64_t len) {
    return (uint64_t)fs_truncate((const char *)(uintptr_t)path_addr, (uint32_t)len);
}

/* ── Raw Ethernet send (SYS_RAW_SEND=216) ──────────────────────── */
static uint64_t sys_raw_send(uint64_t buf_addr, uint64_t len) {
    if (len == 0 || len > 1514) return (uint64_t)-1;
    int r = net_link_send((const uint8_t *)(uintptr_t)buf_addr, (uint32_t)len);
    return r < 0 ? (uint64_t)-1 : len;
}

/* ── FD-based read/write (SYS_FD_READ=217, SYS_FD_WRITE=218) ──── */
static uint64_t sys_fd_read(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used) return (uint64_t)-1;
    uint32_t fsize = 0;
    struct vfs_stat st;
    if (vfs_stat(pfd->path, &st) < 0) return (uint64_t)-1;
    fsize = st.size;
    if (pfd->offset >= fsize) return 0;
    uint32_t avail = fsize - pfd->offset;
    uint32_t to_read = (uint32_t)count < avail ? (uint32_t)count : avail;
    uint32_t need_end = pfd->offset + to_read;
    if (need_end > fsize) need_end = fsize;
    uint8_t *tmp = kmalloc(need_end);
    if (!tmp) return (uint64_t)-1;
    uint32_t nread = 0;
    vfs_read(pfd->path, tmp, need_end, &nread);
    memcpy((void *)(uintptr_t)buf_addr, tmp + pfd->offset, to_read);
    kfree(tmp);
    pfd->offset += to_read;
    return (uint64_t)to_read;
}

static uint64_t sys_fd_write(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used) return (uint64_t)-1;
    int r = vfs_write(pfd->path, (const void *)(uintptr_t)buf_addr, (uint32_t)count);
    if (r >= 0) pfd->offset += (uint32_t)count;
    return (r >= 0) ? count : (uint64_t)-1;
}

/* ── Heap syscalls (malloc/free/calloc/realloc via kmalloc) ───── */

static uint64_t sys_malloc(uint64_t size) {
    return (uint64_t)(uintptr_t)kmalloc((size_t)size);
}

static uint64_t sys_free(uint64_t ptr) {
    kfree((void *)(uintptr_t)ptr);
    return 0;
}

static uint64_t sys_realloc(uint64_t ptr, uint64_t new_size) {
    if (!ptr) return sys_malloc(new_size);
    if (!new_size) { kfree((void *)(uintptr_t)ptr); return 0; }
    void *newp = kmalloc((size_t)new_size);
    if (!newp) return 0;
    size_t copy = (size_t)new_size;
    for (size_t i = 0; i < copy; i++) ((uint8_t *)newp)[i] = ((uint8_t *)(uintptr_t)ptr)[i];
    kfree((void *)(uintptr_t)ptr);
    return (uint64_t)(uintptr_t)newp;
}

static uint64_t sys_calloc(uint64_t nmemb, uint64_t size) {
    if (nmemb != 0 && (size_t)(nmemb * size) / (size_t)nmemb != (size_t)size)
        return 0;
    size_t total = (size_t)(nmemb * size);
    void *p = kmalloc(total);
    if (p) for (size_t i = 0; i < total; i++) ((uint8_t *)p)[i] = 0;
    return (uint64_t)(uintptr_t)p;
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

/* ── TCP server syscalls ─────────────────────────────────────── */

static uint64_t sys_net_tcp_listen(uint64_t port) {
    net_tcp_listen((uint16_t)port,
                   (tcp_connect_handler)0,
                   (tcp_data_handler)0,
                   (tcp_close_handler)0);
    return 0;
}

static uint64_t sys_net_tcp_accept(uint64_t port, uint64_t timeout_ticks) {
    return (uint64_t)(int64_t)net_tcp_accept((uint16_t)port, (int)timeout_ticks);
}

static uint64_t sys_net_tcp_send_conn(uint64_t conn_id, uint64_t buf_addr, uint64_t len) {
    return (uint64_t)(int64_t)net_tcp_send((int)conn_id,
                                           (const void *)buf_addr, (uint16_t)len);
}

static uint64_t sys_net_tcp_recv_conn(uint64_t conn_id, uint64_t buf_addr,
                                      uint64_t len, uint64_t timeout_ticks) {
    return (uint64_t)(int64_t)net_tcp_recv((int)conn_id, (void *)buf_addr,
                                           (uint16_t)len, (int)timeout_ticks);
}

static uint64_t sys_net_tcp_close_conn(uint64_t conn_id) {
    net_tcp_close((int)conn_id);
    return 0;
}

static uint64_t sys_net_tcp_unlisten(uint64_t port) {
    net_tcp_unlisten((uint16_t)port);
    return 0;
}

static uint64_t sys_net_tcp_connect(uint64_t ip, uint64_t port) {
    return (uint64_t)(int64_t)net_tcp_connect((uint32_t)ip, (uint16_t)port);
}

/* ── Mutex syscalls ────────────────────────────────────────────────────────── */
#include "mutex.h"
#include "semaphore.h"

static uint64_t sys_mutex_init(void) {
    return (uint64_t)(int64_t)mutex_init();
}
static uint64_t sys_mutex_lock(uint64_t id) {
    mutex_lock((int)id);
    return 0;
}
static uint64_t sys_mutex_unlock(uint64_t id) {
    mutex_unlock((int)id);
    return 0;
}
static uint64_t sys_mutex_destroy(uint64_t id) {
    mutex_destroy((int)id);
    return 0;
}

/* ── Semaphore syscalls ────────────────────────────────────────────────────── */
static uint64_t sys_sem_init(uint64_t count) {
    return (uint64_t)(int64_t)sem_init((int)count);
}
static uint64_t sys_sem_wait(uint64_t id) {
    sem_wait((int)id);
    return 0;
}
static uint64_t sys_sem_post(uint64_t id) {
    sem_post((int)id);
    return 0;
}
static uint64_t sys_sem_destroy(uint64_t id) {
    sem_destroy((int)id);
    return 0;
}

/* ── UDP server syscalls ──────────────────────────────────────────────────── */
static uint64_t sys_net_udp_listen(uint64_t port) {
    return (uint64_t)(int64_t)net_udp_listen((uint16_t)port);
}
static uint64_t sys_net_udp_recv(uint64_t port, uint64_t buf, uint64_t bufsz,
                                 uint64_t src_ip_ptr, uint64_t src_port_ptr) {
    return (uint64_t)(int64_t)net_udp_recv((uint16_t)port,
        (void *)buf, (uint16_t)bufsz,
        (uint32_t *)src_ip_ptr, (uint16_t *)src_port_ptr, 200);
}
static uint64_t sys_net_udp_unlisten(uint64_t port) {
    net_udp_unlisten((uint16_t)port);
    return 0;
}

/* ── FS extended syscalls ────────────────────────────────────────────────── */
static uint64_t sys_fs_symlink(uint64_t path_addr, uint64_t target_addr) {
    return (uint64_t)(int64_t)fs_symlink((const char *)path_addr,
                                         (const char *)target_addr);
}
static uint64_t sys_fs_readlink(uint64_t path_addr, uint64_t buf_addr, uint64_t bufsz) {
    return (uint64_t)(int64_t)fs_readlink((const char *)path_addr,
                                          (char *)buf_addr, (int)bufsz);
}
static uint64_t sys_fs_lstat(uint64_t path_addr, uint64_t size_addr, uint64_t type_addr) {
    return (uint64_t)(int64_t)fs_lstat((const char *)path_addr,
                                       (uint32_t *)size_addr, (uint8_t *)type_addr);
}

static uint64_t sys_chdir(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    /* Prefer per-session CWD so that cd/pwd work correctly even when
     * net_poll() is called from a different process (e.g. httpd). */
    char *ses_cwd = telnet_get_cwd_ctx();
    /* Resolve to absolute path via VFS */
    char ap[128];
    if (path[0] != '/') {
        const char *base = ses_cwd ? (ses_cwd[0] ? ses_cwd : "/")
                                   : (process_get_current() && process_get_current()->cwd[0]
                                      ? process_get_current()->cwd : "/");
        int cl = (int)strlen(base);
        int pl = (int)strlen(path);
        if (cl + 1 + pl < (int)sizeof(ap)) {
            memcpy(ap, base, cl);
            if (ap[cl-1] != '/') ap[cl++] = '/';
            memcpy(ap + cl, path, pl + 1);
        } else { ap[0] = '/'; ap[1] = '\0'; }
    } else {
        strncpy(ap, path, sizeof(ap)-1); ap[sizeof(ap)-1] = '\0';
    }
    /* Special case: root always exists */
    if (ap[0] == '/' && ap[1] == '\0') {
        if (ses_cwd) { ses_cwd[0] = '/'; ses_cwd[1] = '\0'; return 0; }
        struct process *cur = process_get_current();
        if (!cur) return (uint64_t)(int64_t)-1;
        cur->cwd[0] = '/'; cur->cwd[1] = '\0';
        return 0;
    }
    /* Verify it's a directory */
    struct vfs_stat st;
    int vstat_r = vfs_stat(ap, &st);
    if (vstat_r < 0 || st.type != 2) return (uint64_t)(int64_t)-1;
    /* Remove trailing slash (except root) */
    int len = (int)strlen(ap);
    while (len > 1 && ap[len-1] == '/') { ap[--len] = '\0'; }
    if (ses_cwd) { strncpy(ses_cwd, ap, 63); ses_cwd[63] = '\0'; return 0; }
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)(int64_t)-1;
    strncpy(cur->cwd, ap, 63); cur->cwd[63] = '\0';
    return 0;
}

static uint64_t sys_getcwd(uint64_t buf_addr, uint64_t buf_size) {
    char *ses_cwd = telnet_get_cwd_ctx();
    const char *cwd;
    if (ses_cwd)
        cwd = (ses_cwd[0] != '\0') ? ses_cwd : "/";
    else {
        struct process *cur = process_get_current();
        cwd = (cur && cur->cwd[0]) ? cur->cwd : "/";
    }
    char *buf = (char *)buf_addr;
    if (buf_size == 0) return (uint64_t)-1;
    int max = (int)buf_size;
    strncpy(buf, cwd, max - 1); buf[max-1] = '\0';
    return 0;
}

static uint64_t sys_setpriority(uint64_t pri) {
    if (pri >= 4) return (uint64_t)(int64_t)-1;
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)(int64_t)-1;
    return (uint64_t)(int64_t)scheduler_set_priority(cur, (uint8_t)pri);
}

static uint64_t sys_setpriority_pid(uint64_t pid, uint64_t pri) {
    if (pri >= 4) return (uint64_t)(int64_t)-1;
    struct process *p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-1;
    return (uint64_t)(int64_t)scheduler_set_priority(p, (uint8_t)pri);
}

static uint64_t sys_getpriority(uint64_t pid) {
    struct process *p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-1;
    return p->priority;
}

static uint64_t sys_setpgid(uint64_t pid, uint64_t pgid) {
    struct process *p;
    if (pid == 0) p = process_get_current();
    else p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-1;
    p->pgid = pgid ? (uint32_t)pgid : p->pid;
    if (p->sid == 0) p->sid = p->pgid;
    return 0;
}

static uint64_t sys_getpgid(uint64_t pid) {
    struct process *p;
    if (pid == 0) p = process_get_current();
    else p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-1;
    return p->pgid;
}

static uint64_t sys_killpg(uint64_t pgid, uint64_t sig) {
    return (uint64_t)(int64_t)signal_send_group((uint32_t)pgid, (int)sig);
}

static uint64_t sys_shm_get(uint64_t key) {
    return (uint64_t)(int64_t)shm_get((int)key);
}

static uint64_t sys_shm_at(uint64_t id) {
    return shm_at((int)id);
}

static uint64_t sys_shm_dt(uint64_t id) {
    return (uint64_t)(int64_t)shm_dt((int)id);
}

static uint64_t sys_shm_free(uint64_t id) {
    return (uint64_t)(int64_t)shm_free((int)id);
}

static uint64_t sys_fork(void) {
    return (uint64_t)(int64_t)process_fork();
}

static uint64_t sys_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                           uint64_t tls, uint64_t ctid) {
    (void)ptid; (void)tls; (void)ctid;

    struct process *parent = process_get_current();
    if (!parent) return (uint64_t)-1;

    uint64_t user_rip = syscall_user_rip;
    uint64_t user_rflags = syscall_user_rflags;

    /* For kernel-mode callers: RIP/RFLAGS from syscall_user_* may be stale.
     * Use a default: treat kernel callers differently. */
    if (parent->is_user) {
        int ret = process_clone(parent, flags, (void *)child_stack,
                                user_rip, user_rflags);
        return (uint64_t)(int64_t)ret;
    }

    /* Kernel-mode clone: create a thread that calls a function.
     * child_stack is actually a function pointer for kernel threads. */
    int ret = process_clone(parent, flags, (void *)child_stack,
                            0, 0);
    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_gettid(void) {
    struct process *p = process_get_current();
    if (!p) return 0;
    return (uint64_t)p->tgid ? (uint64_t)p->tgid : (uint64_t)p->pid;
}

static uint64_t sys_tkill(uint64_t pid, uint64_t sig) {
    return (uint64_t)(int64_t)signal_send((uint32_t)pid, (int)sig);
}

static uint64_t sys_execve(uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)-1;
    /* For now, ignore argv/envp */
    (void)argv_addr; (void)envp_addr;
    int ret = process_execve(path, NULL, NULL);
    /* If execve succeeds, we never return here (the process is redirected).
     * If it fails, we return -1. */
    return (uint64_t)(int64_t)ret;
}

/* ── mmap / munmap / mprotect syscalls ──────────────────────── */

static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot) {
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4) return (uint64_t)-1;

    /* Anonymous private mapping only for now */
    if (length == 0) return (uint64_t)-1;

    /* Round up to page size */
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    /* If addr is 0, find a free region starting at USER_CODE_BASE + 2MB */
    if (addr == 0) {
        /* Simple allocation from a growing heap area above the ELF */
        /* Start searching from a reasonable user address */
        addr = 0x0000000001000000ULL;
        /* Scan for a free region of the right size */
        while (addr + length < USER_VADDR_MAX) {
            int free = 1;
            for (uint64_t check = addr; check < addr + length; check += PAGE_SIZE) {
                if (vmm_page_is_mapped_user(proc->pml4, check)) {
                    free = 0;
                    break;
                }
            }
            if (free) break;
            addr += 0x100000ULL; /* 1MB steps */
        }
        if (addr + length >= USER_VADDR_MAX) return (uint64_t)-1;
    }

    uint64_t page_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (prot & 2) page_flags |= VMM_FLAG_WRITE;  /* PROT_WRITE */
    if (vmm_map_user_pages(proc->pml4, addr, length / PAGE_SIZE, page_flags) < 0)
        return (uint64_t)-1;

    if (proc->pml4 == vmm_get_pml4()) {
        /* If this process is running, flush TLB for the new region */
        for (uint64_t v = addr; v < addr + length; v += PAGE_SIZE)
            local_invlpg(v);
    }

    return addr;
}

static uint64_t sys_munmap(uint64_t addr, uint64_t length) {
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4) return (uint64_t)-1;
    if (addr & (PAGE_SIZE - 1)) return (uint64_t)-1;

    if (length == 0) return 0;
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    if (addr + length < addr) return (uint64_t)-1;
    if (addr + length > USER_VADDR_MAX) return (uint64_t)-1;

    if (vmm_unmap_user_pages(proc->pml4, addr, length / PAGE_SIZE) < 0)
        return (uint64_t)-1;
    return 0;
}

static uint64_t sys_mprotect(uint64_t addr, uint64_t length, uint64_t prot) {
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4) return (uint64_t)-1;
    if (addr & (PAGE_SIZE - 1)) return (uint64_t)-1;

    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    if (addr + length < addr) return (uint64_t)-1;
    if (addr + length > USER_VADDR_MAX) return (uint64_t)-1;

    uint64_t page_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (prot & 2) page_flags |= VMM_FLAG_WRITE;  /* PROT_WRITE */
    /* PROT_READ is always set; PROT_EXEC needs NX but we skip that */

    if (vmm_set_user_pages_flags(proc->pml4, addr, length / PAGE_SIZE, page_flags) < 0)
        return (uint64_t)-1;
    return 0;
}

/* ── CPU affinity syscalls ──────────────────────────────────── */

static uint64_t sys_sched_setaffinity(uint64_t pid, uint64_t cpuset) {
    struct process *proc = NULL;
    if (pid == 0) {
        proc = process_get_current();
    } else {
        proc = process_get_by_pid((uint32_t)pid);
    }
    if (!proc) return (uint64_t)-1;
    /* Only the low 8 bits represent CPU affinity; bit 0 = CPU 0, etc. */
    proc->cpu_affinity = (uint8_t)(cpuset & 0xFF);
    return 0;
}

static uint64_t sys_sched_getaffinity(uint64_t pid) {
    struct process *proc = NULL;
    if (pid == 0) {
        proc = process_get_current();
    } else {
        proc = process_get_by_pid((uint32_t)pid);
    }
    if (!proc) return (uint64_t)-1;
    return (uint64_t)proc->cpu_affinity;
}

/* ── dup / dup2 ─────────────────────────────────────────────── */

/* Find lowest available FD slot */
static int fd_find_free(struct process *proc) {
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!proc->fd_table[i].used) return i;
    }
    return -1;
}

static uint64_t sys_dup(uint64_t old_fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return (uint64_t)-1;

    int new_fd = fd_find_free(proc);
    if (new_fd < 0) return (uint64_t)-1;

    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    proc->fd_table[new_fd].offset = proc->fd_table[old_fd].offset;
    return (uint64_t)new_fd;
}

static uint64_t sys_dup2(uint64_t old_fd, uint64_t new_fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return (uint64_t)-1;
    if (new_fd >= PROCESS_FD_MAX) return (uint64_t)-1;

    /* If new_fd is the same as old_fd, just return it */
    if (old_fd == new_fd) return new_fd;

    /* Close new_fd if open */
    if (proc->fd_table[new_fd].used) {
        memset(&proc->fd_table[new_fd], 0, sizeof(struct process_fd));
    }

    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    proc->fd_table[new_fd].offset = proc->fd_table[old_fd].offset;
    return new_fd;
}

/* ── fcntl ──────────────────────────────────────────────────── */

#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (fd >= PROCESS_FD_MAX || !proc->fd_table[fd].used)
        return (uint64_t)-1;

    switch (cmd) {
        case F_DUPFD: {
            /* Duplicate fd to lowest FD >= arg */
            int new_fd = (int)arg;
            if (new_fd < 0) new_fd = 0;
            if (new_fd >= PROCESS_FD_MAX) return (uint64_t)-1;
            while (new_fd < PROCESS_FD_MAX && proc->fd_table[new_fd].used)
                new_fd++;
            if (new_fd >= PROCESS_FD_MAX) return (uint64_t)-1;
            proc->fd_table[new_fd] = proc->fd_table[fd];
            return (uint64_t)new_fd;
        }
        case F_GETFD:
            return (uint64_t)proc->fd_table[fd].flags;
        case F_SETFD:
            proc->fd_table[fd].flags = (uint8_t)arg;
            return 0;
        case F_GETFL:
            /* Return simulated flags (always RDWR for now) */
            return 2; /* O_RDWR */
        case F_SETFL: {
            /* Handle O_NONBLOCK for pipe FDs */
            uint8_t nonblock = (arg & O_NONBLOCK) ? 1 : 0;
            /* Try to find pipe ID from fd path pattern */
            if (strncmp(proc->fd_table[fd].path, "pipe_", 5) == 0) {
                int pid = (int)proc->fd_table[fd].offset;
                pipe_set_nonblock(pid, nonblock);
            }
            return 0;
        }
        default:
            return (uint64_t)-1;
    }
}

/* ── select (I/O multiplexing) ──────────────────────────────── */

/* Maximum select timeout in ticks (about 1 second at 100Hz) when blocking.
 * For simplicity, we yield and re-check. */
#define SELECT_MAX_TICKS 1

static uint64_t sys_select(uint64_t nfds, uint64_t readfds_addr,
                            uint64_t writefds_addr, uint64_t exceptfds_addr,
                            uint64_t timeout_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;

    fd_set readfds, writefds, exceptfds;
    fd_set orig_readfds, orig_writefds, orig_exceptfds;

    /* Copy in from userspace — skip NULL sets */
    if (readfds_addr) {
        memcpy(&orig_readfds, (void*)readfds_addr, sizeof(fd_set));
    } else {
        FD_ZERO(&orig_readfds);
    }
    if (writefds_addr) {
        memcpy(&orig_writefds, (void*)writefds_addr, sizeof(fd_set));
    } else {
        FD_ZERO(&orig_writefds);
    }
    if (exceptfds_addr) {
        memcpy(&orig_exceptfds, (void*)exceptfds_addr, sizeof(fd_set));
    } else {
        FD_ZERO(&orig_exceptfds);
    }

    uint64_t timeout = 0;
    if (timeout_addr) {
        struct timespec ts;
        memcpy(&ts, (void*)timeout_addr, sizeof(ts));
        /* Convert to ticks */
        timeout = ts.tv_sec * 100 + ts.tv_nsec / 10000000;
    }

    int loops = 0;
    int max_loops = (timeout == 0) ? 1 : (int)(timeout / SELECT_MAX_TICKS);
    if (max_loops < 1) max_loops = 1;

    int ready = 0;
    while (loops < max_loops) {
        ready = 0;
        if (readfds_addr) {
            memcpy(&readfds, &orig_readfds, sizeof(fd_set));
            /* Check each FD for readability */
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &readfds)) continue;
                if (i >= PROCESS_FD_MAX || !proc->fd_table[i].used) {
                    /* Bad FD — remove from set */
                    FD_CLR(i, &readfds);
                    continue;
                }
                /* For now, pipes are readable if they have data */
                /* For real implementation, check pipe count and other sources */
                /* Simple: always mark as readable */
                /* In a full implementation, check pipe_available() etc. */
            }
        }
        if (writefds_addr) {
            memcpy(&writefds, &orig_writefds, sizeof(fd_set));
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &writefds)) continue;
                if (i >= PROCESS_FD_MAX || !proc->fd_table[i].used) {
                    FD_CLR(i, &writefds);
                    continue;
                }
                /* Most FDs are writable unless pipe is full */
            }
        }
        if (exceptfds_addr) {
            (void)0; /* not implemented */
        }

        /* Count ready FDs */
        int total = 0;
        for (int i = 0; i < (int)nfds; i++) {
            if ((readfds_addr && FD_ISSET(i, &readfds)) ||
                (writefds_addr && FD_ISSET(i, &writefds)))
                total++;
        }

        if (total > 0) {
            /* Copy results back */
            if (readfds_addr) memcpy((void*)readfds_addr, &readfds, sizeof(fd_set));
            if (writefds_addr) memcpy((void*)writefds_addr, &writefds, sizeof(fd_set));
            if (exceptfds_addr) memcpy((void*)exceptfds_addr, &exceptfds, sizeof(fd_set));
            return (uint64_t)total;
        }

        if (timeout == 0) break; /* Non-blocking */
        loops++;

        /* Yield and try again */
        scheduler_yield();
    }

    /* Timeout — return 0 */
    if (readfds_addr) memcpy((void*)readfds_addr, &readfds, sizeof(fd_set));
    if (writefds_addr) memcpy((void*)writefds_addr, &writefds, sizeof(fd_set));
    if (exceptfds_addr) memcpy((void*)exceptfds_addr, &exceptfds, sizeof(fd_set));
    return 0;
}

/* ── setitimer / getitimer (per-process interval timers) ────── */

/* SIGALRM */
#ifndef SIGALRM
#define SIGALRM 14
#endif

/* Called from timer tick to decrement per-process timers */
void process_timer_tick(void) {
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &table[i];
        if (p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE)
            continue;
        for (int t = 0; t < ITIMER_MAX; t++) {
            if (p->itimers[t].it_value > 0) {
                p->itimers[t].it_value--;
                if (p->itimers[t].it_value == 0) {
                    /* Timer expired — send signal and reload */
                    if (t == ITIMER_REAL)
                        signal_send(p->pid, SIGALRM);
                    /* Reload interval if periodic */
                    p->itimers[t].it_value = p->itimers[t].it_interval;
                }
            }
        }
    }
}

static uint64_t sys_setitimer(uint64_t which, uint64_t new_val_addr,
                               uint64_t old_val_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (which >= ITIMER_MAX) return (uint64_t)-1;

    struct itimerval new_val;
    memset(&new_val, 0, sizeof(new_val));

    if (new_val_addr) {
        memcpy(&new_val, (void*)new_val_addr, sizeof(struct itimerval));
    }

    /* Return old value if requested */
    if (old_val_addr) {
        memcpy((void*)old_val_addr, &proc->itimers[which], sizeof(struct itimerval));
    }

    /* Set new value */
    proc->itimers[which] = new_val;
    return 0;
}

static uint64_t sys_getitimer(uint64_t which, uint64_t cur_val_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (which >= ITIMER_MAX) return (uint64_t)-1;
    if (!cur_val_addr) return (uint64_t)-1;

    memcpy((void*)cur_val_addr, &proc->itimers[which], sizeof(struct itimerval));
    return 0;
}

/* ── nanosleep ──────────────────────────────────────────────── */

static uint64_t sys_nanosleep(uint64_t req_addr, uint64_t rem_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (!req_addr) return (uint64_t)-1;

    struct timespec req;
    memcpy(&req, (void*)req_addr, sizeof(req));

    /* Convert to ticks */
    uint64_t ticks = req.tv_sec * 100 + req.tv_nsec / 10000000;
    if (ticks == 0 && req.tv_nsec > 0) ticks = 1; /* minimum 1 tick */

    /* Block by setting sleep_until */
    uint64_t now = timer_get_ticks();
    proc->sleep_until = now + ticks;
    proc->state = PROCESS_BLOCKED;
    scheduler_remove(proc);
    scheduler_yield();

    /* Process woke up — check if it was from timer or signal */
    if (rem_addr && timer_get_ticks() < proc->sleep_until) {
        /* Woke early (signal) — compute remaining */
        struct timespec rem;
        uint64_t remaining = proc->sleep_until - timer_get_ticks();
        rem.tv_sec = remaining / 100;
        rem.tv_nsec = (remaining % 100) * 10000000;
        memcpy((void*)rem_addr, &rem, sizeof(rem));
    }

    return 0;
}

/* ── sysconf ────────────────────────────────────────────────── */

#define _SC_CLK_TCK       2
#define _SC_PAGESIZE      30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84

static uint64_t sys_sysconf(uint64_t name) {
    switch (name) {
        case _SC_CLK_TCK:
            return 100;  /* PIT frequency */
        case _SC_PAGESIZE:
            return PAGE_SIZE;
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN:
            return (uint64_t)smp_get_cpu_count();
        default:
            return (uint64_t)-1;
    }
}

/* ── uname ──────────────────────────────────────────────────── */

static uint64_t sys_uname(uint64_t buf_addr) {
    if (!buf_addr) return (uint64_t)-1;
    struct utsname *buf = (struct utsname *)buf_addr;

    /* Verify user pointer */

    memset(buf, 0, sizeof(struct utsname));
    memcpy(buf->sysname, "OS", 3);
    memcpy(buf->nodename, "localhost", 10);
    memcpy(buf->release, "1.0.0", 6);
    memcpy(buf->version, __DATE__, 12);
    memcpy(buf->machine, "x86_64", 7);
    return 0;
}

/* ── Global hostname ─────────────────────────────────────────── */

#define HOSTNAME_MAX 64
static char system_hostname[HOSTNAME_MAX] = "os";

/* ── pipe() ──────────────────────────────────────────────────── */

static uint64_t sys_pipe(uint64_t fds_addr) {
    if (!fds_addr) return (uint64_t)-1;
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    int id = pipe_create();
    if (id < 0) return (uint64_t)-1;

    /* Allocate two FD slots */
    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!proc->fd_table[i].used) {
            if (read_fd < 0) read_fd = i;
            else if (write_fd < 0) { write_fd = i; break; }
        }
    }
    if (read_fd < 0 || write_fd < 0) return (uint64_t)-1;

    /* Store pipe index as part of fd path */
    proc->fd_table[read_fd].used = 1;
    proc->fd_table[read_fd].offset = (uint32_t)id; /* store pipe id */
    snprintf(proc->fd_table[read_fd].path, 64, "pipe_read_%d", id);
    proc->fd_table[read_fd].flags = 0;

    proc->fd_table[write_fd].used = 1;
    proc->fd_table[write_fd].offset = (uint32_t)id;
    snprintf(proc->fd_table[write_fd].path, 64, "pipe_write_%d", id);
    proc->fd_table[write_fd].flags = 0;

    /* Write fds back to userspace */
    uint32_t fds[2] = { (uint32_t)read_fd, (uint32_t)write_fd };
    memcpy((void*)fds_addr, fds, sizeof(fds));
    return 0;
}

/* ── getppid() ───────────────────────────────────────────────── */

static uint64_t sys_getppid(void) {
    struct process *proc = process_get_current();
    if (!proc) return 0;
    return (uint64_t)proc->parent_pid;
}

/* ── alarm() ─────────────────────────────────────────────────── */

static uint64_t sys_alarm(uint64_t seconds) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    /* Convert seconds to ticks (100 Hz) */
    uint64_t ticks = seconds * 100;
    uint64_t old_value = 0;

    /* Get old value */
    if (proc->itimers[ITIMER_REAL].it_value > 0)
        old_value = proc->itimers[ITIMER_REAL].it_value / 100;

    /* Set new alarm */
    proc->itimers[ITIMER_REAL].it_value = ticks;
    proc->itimers[ITIMER_REAL].it_interval = 0; /* one-shot */

    return old_value;
}

/* ── pause() ─────────────────────────────────────────────────── */

static uint64_t sys_pause(void) {
    /* Block the current process until a signal arrives */
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    proc->state = PROCESS_BLOCKED;
    scheduler_remove(proc);
    scheduler_yield();

    /* Woken by signal — return -1 (always interrupted) */
    return (uint64_t)-1;
}

/* ── access() ────────────────────────────────────────────────── */

static uint64_t sys_access(uint64_t path_addr, uint64_t mode) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)-1;

    /* Check if file exists */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0) return (uint64_t)-1;

    /* For now, we don't check permissions (always OK if file exists) */
    (void)mode;
    return 0;
}

/* ── getuid / geteuid / getgid / getegid ────────────────────── */

static uint64_t sys_getuid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    return (uint64_t)proc->uid;
}

static uint64_t sys_geteuid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    return (uint64_t)proc->euid;
}

static uint64_t sys_getgid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    return (uint64_t)proc->gid;
}

static uint64_t sys_getegid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    return (uint64_t)proc->egid;
}

/* ── rmdir() ─────────────────────────────────────────────────── */

static uint64_t sys_rmdir(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)-1;

    /* Use VFS unlink (same as delete for directories) */
    if (vfs_unlink(path) < 0) return (uint64_t)-1;
    return 0;
}

/* ── rename() ────────────────────────────────────────────────── */

static uint64_t sys_rename(uint64_t old_addr, uint64_t new_addr) {
    const char *old_path = (const char *)old_addr;
    const char *new_path = (const char *)new_addr;
    if (!old_path || !new_path) return (uint64_t)-1;

    /* Simple rename: create new, copy data, delete old.
     * For files only; directories not supported yet. */
    struct vfs_stat st;
    if (vfs_stat(old_path, &st) < 0) return (uint64_t)-1;

    /* Read old file */
    uint8_t *buf = (uint8_t *)kmalloc(st.size + 1);
    if (!buf) return (uint64_t)-1;
    uint32_t sz = 0;
    if (vfs_read(old_path, buf, st.size, &sz) < 0) {
        kfree(buf);
        return (uint64_t)-1;
    }

    /* Create new file */
    if (vfs_create(new_path, st.type) < 0) {
        kfree(buf);
        return (uint64_t)-1;
    }

    /* Write data */
    if (st.size > 0 && vfs_write(new_path, buf, st.size) < 0) {
        kfree(buf);
        vfs_unlink(new_path);
        return (uint64_t)-1;
    }
    kfree(buf);

    /* Delete old */
    vfs_unlink(old_path);
    return 0;
}

/* ── chmod() ─────────────────────────────────────────────────── */

static uint64_t sys_chmod(uint64_t path_addr, uint64_t mode) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)-1;
    return (uint64_t)fs_chmod(path, (uint16_t)mode);
}

/* ── fsync() ─────────────────────────────────────────────────── */

static uint64_t sys_fsync(uint64_t fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (fd >= PROCESS_FD_MAX || !proc->fd_table[fd].used)
        return (uint64_t)-1;

    /* For now, assume data is written through. In a real OS this would
     * flush disk caches. We just validate the fd and return success. */
    (void)fd;
    return 0;
}

/* ── sigprocmask / sigpending ────────────────────────────────── */

static uint64_t sys_sigprocmask(uint64_t how, uint64_t set_addr, uint64_t oldset_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    /* Return old mask */
    if (oldset_addr) {
        uint32_t old = proc->sig_mask;
        memcpy((void*)oldset_addr, &old, sizeof(old));
    }

    /* Apply new mask */
    if (set_addr) {
        uint32_t new_mask = 0;
        memcpy(&new_mask, (void*)set_addr, sizeof(new_mask));

        switch (how) {
            case SIG_BLOCK:
                proc->sig_mask |= new_mask;
                break;
            case SIG_UNBLOCK:
                proc->sig_mask &= ~new_mask;
                break;
            case SIG_SETMASK:
                proc->sig_mask = new_mask;
                break;
            default:
                return (uint64_t)-1;
        }
    }

    return 0;
}

static uint64_t sys_sigpending(uint64_t set_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (!set_addr) return (uint64_t)-1;

    memcpy((void*)set_addr, &proc->pending_signals, sizeof(uint32_t));
    return 0;
}

/* ── readv / writev (vectored I/O) ──────────────────────────── */

static uint64_t sys_readv(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt) {
    if (!iov_addr || iovcnt == 0) return 0;
    if (iovcnt > 16) iovcnt = 16; /* sanity */

    struct iovec iov[16];
    memcpy(iov, (void*)iov_addr, sizeof(struct iovec) * iovcnt);

    uint64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base && iov[i].iov_len > 0) {
            int64_t n = (int64_t)sys_read(fd, (uint64_t)iov[i].iov_base,
                                          iov[i].iov_len);
            if (n < 0) return total ? total : (uint64_t)-1;
            total += (uint64_t)n;
            if ((uint64_t)n < iov[i].iov_len) break; /* short read */
        }
    }
    return total;
}

static uint64_t sys_writev(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt) {
    if (!iov_addr || iovcnt == 0) return 0;
    if (iovcnt > 16) iovcnt = 16;

    struct iovec iov[16];
    memcpy(iov, (void*)iov_addr, sizeof(struct iovec) * iovcnt);

    uint64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_base && iov[i].iov_len > 0) {
            int64_t n = (int64_t)sys_write(fd, (uint64_t)iov[i].iov_base,
                                           iov[i].iov_len);
            if (n < 0) return total ? total : (uint64_t)-1;
            total += (uint64_t)n;
        }
    }
    return total;
}

/* ── getrandom / PRNG ────────────────────────────────────────── */

/* xorshift64 PRNG state */
static uint64_t prng_state = 0xDEADBEEFCAFEBABEULL;

static uint64_t xorshift64(void) {
    uint64_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prng_state = x;
    return x;
}

static uint64_t sys_getrandom(uint64_t buf_addr, uint64_t count,
                               uint64_t flags) {
    if (!buf_addr || count == 0) return 0;
    if (count > 4096) count = 4096; /* limit per call */

    uint8_t *buf = (uint8_t *)buf_addr;
    for (uint64_t i = 0; i < count; i++) {
        buf[i] = (uint8_t)(xorshift64() >> 56);
    }

    (void)flags;
    return count;
}

/* ── reboot() ────────────────────────────────────────────────── */

static uint64_t sys_reboot(void) {
    /* Call ACPI shutdown */
    acpi_shutdown();
    /* Should not reach here */
    for (;;) __asm__ volatile("hlt");
    return (uint64_t)-1;
}

/* ── sethostname / gethostname ───────────────────────────────── */

static uint64_t sys_sethostname(uint64_t name_addr, uint64_t len) {
    if (!name_addr) return (uint64_t)-1;
    if (len > HOSTNAME_MAX - 1) len = HOSTNAME_MAX - 1;
    memcpy(system_hostname, (void*)name_addr, (size_t)len);
    system_hostname[len] = '\0';
    return 0;
}

static uint64_t sys_gethostname(uint64_t name_addr, uint64_t len) {
    if (!name_addr || len == 0) return (uint64_t)-1;
    size_t slen = strlen(system_hostname);
    if (slen > (size_t)len - 1) slen = (size_t)len - 1;
    memcpy((void*)name_addr, system_hostname, slen);
    ((char*)name_addr)[slen] = '\0';
    return 0;
}

/* ── umask() ─────────────────────────────────────────────────── */

static uint64_t sys_umask(uint64_t mask) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    uint16_t old = proc->umask;
    proc->umask = (uint16_t)(mask & 0777);
    return (uint64_t)old;
}

/* ── mknod() ─────────────────────────────────────────────────── */

static uint64_t sys_mknod(uint64_t path_addr, uint64_t mode, uint64_t dev) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)-1;

    /* Simple: just create an empty file (FIFOs/devices not supported yet) */
    /* For named FIFOs we could create a pipe-backed file */
    if (vfs_create(path, FS_TYPE_FILE) < 0)
        return (uint64_t)-1;

    (void)mode;
    (void)dev;
    return 0;
}

static void netstat_tcp_cb(uint16_t lport, uint32_t rip, uint16_t rport, int state) {
    const char *snames[] = {"CLOSED","LISTEN","SYN_SENT","SYN_RCV","ESTABLISHED","FIN_WAIT","CLOSE_WAIT","TIME_WAIT"};
    const char *sname = (state >= 0 && state < 8) ? snames[state] : "?";
    kprintf("  TCP  %5u  %u.%u.%u.%u:%u  %s\n",
        (uint64_t)lport,
        (uint64_t)((rip >> 24) & 0xFF), (uint64_t)((rip >> 16) & 0xFF),
        (uint64_t)((rip >>  8) & 0xFF), (uint64_t)(rip & 0xFF),
        (uint64_t)rport, sname);
}

static void netstat_udp_cb(uint16_t port) {
    kprintf("  UDP  %5u  *:*  LISTEN\n", (uint64_t)port);
}

static uint64_t sys_net_connlist(void) {
    kprintf("Proto  LPort  Remote          State\n");
    net_conn_list(netstat_tcp_cb);
    net_udp_list(netstat_udp_cb);
    return 0;
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
        out[written].pgid = table[i].pgid;
        out[written].sid = table[i].sid;
        out[written].state = (uint8_t)table[i].state;
        out[written].is_user = (uint8_t)(table[i].is_user ? 1 : 0);
        out[written].is_background = (uint8_t)(table[i].is_background ? 1 : 0);
        out[written].is_suspended = (uint8_t)(table[i].is_suspended ? 1 : 0);
        out[written].priority = table[i].priority;
        /* Only dereference kernel-space name pointers (bit 63 set).
         * User processes may have a name pointer from user address space
         * which is not accessible in the kernel page table context. */
        if (table[i].name && ((uint64_t)table[i].name >> 63)) {
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

static uint64_t sys_fat_write_file(uint64_t path_addr, uint64_t data_addr, uint64_t size) {
    return (uint64_t)fat32_write_file((const char *)path_addr, (const void *)data_addr, (uint32_t)size);
}

static uint64_t sys_fat_sync(void) {
    return (uint64_t)fat32_sync();
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

/* Single static workspace avoids ~7MB heap alloc/free churn per cc invocation.
 * Protected by a mutex to prevent concurrent access. */
static CompilerState cc_workspace;
static int cc_mutex = -1;

#define CC_INCLUDE_MAX_DEPTH 16
#define CC_PATH_MAX_LEN 256

static int cc_append_src(CompilerState *st, const char *buf, uint32_t n) {
    if (st->src_len + n >= CC_SRC_MAX) return -1;
    memcpy(st->src + st->src_len, buf, n);
    st->src_len += n;
    st->src[st->src_len] = '\0';
    return 0;
}

static void cc_path_dirname(const char *path, char out[CC_PATH_MAX_LEN]) {
    int last_slash = -1;
    int i = 0;
    while (path[i] && i < CC_PATH_MAX_LEN - 1) {
        if (path[i] == '/') last_slash = i;
        i++;
    }
    if (last_slash <= 0) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    memcpy(out, path, (uint32_t)last_slash);
    out[last_slash] = '\0';
}

static int cc_path_join(const char *dir, const char *name, char out[CC_PATH_MAX_LEN]) {
    int di = 0;
    if (name[0] == '/') {
        while (name[di] && di < CC_PATH_MAX_LEN - 1) {
            out[di] = name[di];
            di++;
        }
        out[di] = '\0';
        return 0;
    }

    while (dir[di] && di < CC_PATH_MAX_LEN - 1) {
        out[di] = dir[di];
        di++;
    }
    if (di == 0 || out[di - 1] != '/') {
        if (di >= CC_PATH_MAX_LEN - 1) return -1;
        out[di++] = '/';
    }
    int ni = 0;
    while (name[ni] && di < CC_PATH_MAX_LEN - 1) {
        out[di++] = name[ni++];
    }
    out[di] = '\0';
    return name[ni] ? -1 : 0;
}

static int cc_load_with_includes(CompilerState *st, const char *path, int depth);

static int cc_load_with_includes(CompilerState *st, const char *path, int depth) {
    if (depth > CC_INCLUDE_MAX_DEPTH) return -1;

    char *fbuf = (char *)kmalloc(CC_SRC_MAX);
    if (!fbuf) return -1;
    uint32_t sz = 0;
    int rr = vfs_read(path, fbuf, CC_SRC_MAX - 1, &sz);
    if (rr < 0 || sz == 0) {
        kfree(fbuf);
        return -1;
    }
    fbuf[sz] = '\0';

    char base[CC_PATH_MAX_LEN];
    cc_path_dirname(path, base);

    uint32_t i = 0;
    while (i < sz) {
        uint32_t line_start = i;
        while (i < sz && fbuf[i] != '\n') i++;
        uint32_t line_end = i;
        if (i < sz && fbuf[i] == '\n') i++;

        uint32_t p = line_start;
        while (p < line_end && (fbuf[p] == ' ' || fbuf[p] == '\t')) p++;

        if (p < line_end && fbuf[p] == '#') {
            p++;
            while (p < line_end && (fbuf[p] == ' ' || fbuf[p] == '\t')) p++;

            char dirkw[16] = {0};
            int dk = 0;
            while (p < line_end && ((fbuf[p] >= 'a' && fbuf[p] <= 'z') ||
                   (fbuf[p] >= 'A' && fbuf[p] <= 'Z')) && dk < 15) {
                dirkw[dk++] = fbuf[p++];
            }
            dirkw[dk] = '\0';

            if (strcmp(dirkw, "include") == 0) {
                while (p < line_end && (fbuf[p] == ' ' || fbuf[p] == '\t')) p++;
                if (p < line_end && fbuf[p] == '"') {
                    p++;
                    char inc[CC_PATH_MAX_LEN] = {0};
                    int ii = 0;
                    while (p < line_end && fbuf[p] != '"' && ii < CC_PATH_MAX_LEN - 1)
                        inc[ii++] = fbuf[p++];
                    inc[ii] = '\0';

                    char full[CC_PATH_MAX_LEN] = {0};
                    if (cc_path_join(base, inc, full) < 0) {
                        kfree(fbuf);
                        return -1;
                    }
                    if (cc_load_with_includes(st, full, depth + 1) < 0) {
                        kfree(fbuf);
                        return -1;
                    }
                    continue;
                }
            }
        }

        if (cc_append_src(st, fbuf + line_start, line_end - line_start) < 0 ||
            cc_append_src(st, "\n", 1) < 0) {
            kfree(fbuf);
            return -1;
        }
    }

    kfree(fbuf);
    return 0;
}

static uint64_t sys_cc_compile(uint64_t inpath_addr, uint64_t outpath_addr) {
    const char *inpath = (const char *)inpath_addr;
    const char *outpath = (const char *)outpath_addr;
    if (!inpath || !outpath) return (uint64_t)-1;

    if (cc_mutex >= 0) mutex_lock(cc_mutex);
    CompilerState *cc = &cc_workspace;
    memset(cc, 0, sizeof(CompilerState));
    cc->src_len = 0;
    cc->src[0] = '\0';

    if (cc_load_with_includes(cc, inpath, 0) < 0 || cc->src_len == 0) {
        if (cc_mutex >= 0) mutex_unlock(cc_mutex);
        return (uint64_t)-2;
    }

    cc_lex(cc);
    if (cc->error) {
        kprintf("cc: lex error: %s\n", cc->errmsg);
        if (cc_mutex >= 0) mutex_unlock(cc_mutex);
        return (uint64_t)-3;
    }

    cc_parse(cc);
    if (cc->error) {
        kprintf("cc: error: %s\n", cc->errmsg);
        if (cc_mutex >= 0) mutex_unlock(cc_mutex);
        return (uint64_t)-4;
    }

    int ret = 0;
    if (cc_write_elf(cc, outpath) < 0)
        ret = -5;

    if (cc_mutex >= 0) mutex_unlock(cc_mutex);
    return ret;
}

static uint64_t sys_cc_compile_obj(uint64_t inpath_addr, uint64_t outpath_addr) {
    const char *inpath = (const char *)inpath_addr;
    const char *outpath = (const char *)outpath_addr;
    if (!inpath || !outpath) return (uint64_t)-1;

    if (cc_mutex >= 0) mutex_lock(cc_mutex);
    CompilerState *cc = &cc_workspace;
    memset(cc, 0, sizeof(CompilerState));
    cc->src_len = 0;
    cc->src[0] = '\0';
    cc->obj_mode = 1;

    if (cc_load_with_includes(cc, inpath, 0) < 0 || cc->src_len == 0) {
        if (cc_mutex >= 0) mutex_unlock(cc_mutex);
        return (uint64_t)-2;
    }

    cc_lex(cc);
    if (cc->error) {
        kprintf("cc: lex error: %s\n", cc->errmsg);
        if (cc_mutex >= 0) mutex_unlock(cc_mutex);
        return (uint64_t)-3;
    }

    cc_parse(cc);
    if (cc->error) {
        kprintf("cc: error: %s\n", cc->errmsg);
        if (cc_mutex >= 0) mutex_unlock(cc_mutex);
        return (uint64_t)-4;
    }

    int ret = 0;
    if (cc_write_obj(cc, outpath) < 0)
        ret = -5;

    if (cc_mutex >= 0) mutex_unlock(cc_mutex);
    return ret;
}

static uint64_t sys_cc_link(uint64_t obj_paths_addr, uint64_t nobj, uint64_t outpath_addr) {
    const char **obj_paths = (const char **)obj_paths_addr;
    const char *outpath = (const char *)outpath_addr;
    if (!obj_paths || !outpath || nobj == 0 || nobj > 256) return (uint64_t)-1;

    int ret = cc_link(obj_paths, (int)nobj, outpath, CC_LOAD_BASE);
    return (uint64_t)(int64_t)ret;
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

static struct process *g_doom_proc = NULL;

static uint64_t sys_doom_run(void) {
    if (g_doom_proc && g_doom_proc->state != PROCESS_ZOMBIE &&
        g_doom_proc->state != PROCESS_UNUSED) {
        return (uint64_t)-2;
    }
    if (!vga_is_framebuffer()) {
        if (vga_try_alloc_software_framebuffer() != 0)
            return (uint64_t)-1;
    }
    g_doom_proc = process_create(doom_task, "doom");
    if (!g_doom_proc)
        return (uint64_t)-1;
    int status = 0;
    process_waitpid(g_doom_proc->pid, &status);
    g_doom_proc = NULL;
    keyboard_reset_state();
    vga_refresh_console();
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
        case SYS_NET_TCP_LISTEN:     return sys_net_tcp_listen(a1);
        case SYS_NET_TCP_ACCEPT:     return sys_net_tcp_accept(a1, a2);
        case SYS_NET_TCP_SEND_CONN:  return sys_net_tcp_send_conn(a1, a2, a3);
        case SYS_NET_TCP_RECV_CONN:  return sys_net_tcp_recv_conn(a1, a2, a3, a4);
        case SYS_NET_TCP_CLOSE_CONN: return sys_net_tcp_close_conn(a1);
        case SYS_NET_TCP_UNLISTEN:   return sys_net_tcp_unlisten(a1);
        case SYS_NET_TCP_CONNECT:    return sys_net_tcp_connect(a1, a2);
        case SYS_MUTEX_INIT:         return sys_mutex_init();
        case SYS_MUTEX_LOCK:         return sys_mutex_lock(a1);
        case SYS_MUTEX_UNLOCK:       return sys_mutex_unlock(a1);
        case SYS_MUTEX_DESTROY:      return sys_mutex_destroy(a1);
        case SYS_SEM_INIT:           return sys_sem_init(a1);
        case SYS_SEM_WAIT:           return sys_sem_wait(a1);
        case SYS_SEM_POST:           return sys_sem_post(a1);
        case SYS_SEM_DESTROY:        return sys_sem_destroy(a1);
        case SYS_NET_UDP_LISTEN:      return sys_net_udp_listen(a1);
        case SYS_NET_UDP_RECV:        return sys_net_udp_recv(a1, a2, a3, a4, a5);
        case SYS_NET_UDP_UNLISTEN:    return sys_net_udp_unlisten(a1);
        case SYS_FS_SYMLINK:          return sys_fs_symlink(a1, a2);
        case SYS_FS_READLINK:         return sys_fs_readlink(a1, a2, a3);
        case SYS_FS_LSTAT:            return sys_fs_lstat(a1, a2, a3);
        case SYS_CHDIR:               return sys_chdir(a1);
        case SYS_GETCWD:              return sys_getcwd(a1, a2);
        case SYS_SETPRIORITY:         return sys_setpriority(a1);
        case SYS_SHM_GET:             return sys_shm_get(a1);
        case SYS_SHM_AT:              return sys_shm_at(a1);
        case SYS_SHM_DT:              return sys_shm_dt(a1);
        case SYS_SHM_FREE:            return sys_shm_free(a1);
        case SYS_FORK:                return sys_fork();
        case SYS_CLONE:               return sys_clone(a1, a2, a3, a4, a5);
        case SYS_GETTID:              return sys_gettid();
        case SYS_TKILL:               return sys_tkill(a1, a2);
        case SYS_EXECVE:              return sys_execve(a1, a2, a3);
        case SYS_NET_CONNLIST:         return sys_net_connlist();
        case SYS_SIGNAL:              return sys_signal(a1, a2);
        case SYS_LSEEK:               return sys_lseek(a1, a2, a3);
        case SYS_TRUNCATE:            return sys_truncate(a1, a2);
        case SYS_RAW_SEND:            return sys_raw_send(a1, a2);
        case SYS_FD_READ:             return sys_fd_read(a1, a2, a3);
        case SYS_FD_WRITE:            return sys_fd_write(a1, a2, a3);
        case SYS_SETPRIORITY_PID:     return sys_setpriority_pid(a1, a2);
        case SYS_GETPRIORITY:         return sys_getpriority(a1);
        case SYS_SETPGID:             return sys_setpgid(a1, a2);
        case SYS_GETPGID:             return sys_getpgid(a1);
        case SYS_KILLPG:              return sys_killpg(a1, a2);
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
        case SYS_FAT_WRITE_FILE: return sys_fat_write_file(a1, a2, a3);
        case SYS_FAT_SYNC: return sys_fat_sync();
        case SYS_SHELL_HISTORY_SHOW: return sys_shell_history_show();
        case SYS_SHELL_READ_LINE: return sys_shell_read_line(a1, a2);
        case SYS_SHELL_VAR_SET: return sys_shell_var_set(a1, a2);
        case SYS_SHELL_EXEC_CMD: return sys_shell_exec_cmd(a1, a2);
        case SYS_VGA_SET_COLOR: return sys_vga_set_color(a1, a2);
        case SYS_VGA_GET_FB_INFO: return sys_vga_get_fb_info(a1);
        case SYS_CC_COMPILE: return sys_cc_compile(a1, a2);
        case SYS_CC_COMPILE_OBJ: return sys_cc_compile_obj(a1, a2);
        case SYS_CC_LINK: return sys_cc_link(a1, a2, a3);
        case SYS_KEYBOARD_GETCHAR: return sys_keyboard_getchar();
        case SYS_SHELL_HISTORY_ADD: return sys_shell_history_add(a1);
        case SYS_SHELL_HISTORY_COUNT: return sys_shell_history_count();
        case SYS_SHELL_HISTORY_ENTRY: return sys_shell_history_entry(a1);
        case SYS_SHELL_TAB_COMPLETE: return sys_shell_tab_complete(a1, a2, a3);
        case SYS_VGA_PUT_ENTRY_AT: return sys_vga_put_entry_at(a1, a2, a3, a4);
        case SYS_VGA_SET_CURSOR: return sys_vga_set_cursor(a1, a2);
        case SYS_VGA_CLEAR: return sys_vga_clear();
        case SYS_GUI_SHELL_RUN: return sys_gui_shell_run();
        case SYS_DOOM_RUN: return sys_doom_run();
        case SYS_AC97_PRESENT: return ac97_present() ? 1 : 0;
        case SYS_AC97_BEEP: {
            if (!ac97_present()) return (uint64_t)-1;
            uint32_t freq = (uint32_t)a1;
            uint32_t ms   = (uint32_t)a2;
            if (freq < 100) freq = 440;
            if (ms == 0) ms = 100;
            uint32_t n = (freq * ms) / 1000;
            if (n > 512) n = 512;
            static int16_t pcm[512];
            for (uint32_t i = 0; i < n; i++)
                pcm[i] = (int16_t)(2000 * ((i & 1) ? 1 : -1));
            ac97_play_pcm(pcm, n * sizeof(int16_t), freq);
            return 0;
        }
        case SYS_MALLOC:  return sys_malloc(a1);
        case SYS_FREE:    return sys_free(a1);
        case SYS_REALLOC: return sys_realloc(a1, a2);
        case SYS_CALLOC:  return sys_calloc(a1, a2);
        case SYS_MMAP:    return sys_mmap(a1, a2, a3);
        case SYS_MUNMAP:  return sys_munmap(a1, a2);
        case SYS_MPROTECT: return sys_mprotect(a1, a2, a3);
        case SYS_SCHED_SETAFFINITY: return sys_sched_setaffinity(a1, a2);
        case SYS_SCHED_GETAFFINITY: return sys_sched_getaffinity(a1);
        case SYS_DUP:    return sys_dup(a1);
        case SYS_DUP2:   return sys_dup2(a1, a2);
        case SYS_FCNTL:  return sys_fcntl(a1, a2, a3);
        case SYS_SELECT: return sys_select(a1, a2, a3, a4, a5);
        case SYS_SETITIMER: return sys_setitimer(a1, a2, a3);
        case SYS_GETITIMER: return sys_getitimer(a1, a2);
        case SYS_NANOSLEEP: return sys_nanosleep(a1, a2);
        case SYS_SYSCONF: return sys_sysconf(a1);
        case SYS_UNAME:  return sys_uname(a1);
        case SYS_PIPE:        return sys_pipe(a1);
        case SYS_GETPPID:     return sys_getppid();
        case SYS_ALARM:       return sys_alarm(a1);
        case SYS_PAUSE:       return sys_pause();
        case SYS_ACCESS:      return sys_access(a1, a2);
        case SYS_GETUID:      return sys_getuid();
        case SYS_GETEUID:     return sys_geteuid();
        case SYS_GETGID:      return sys_getgid();
        case SYS_GETEGID:     return sys_getegid();
        case SYS_RMDIR:       return sys_rmdir(a1);
        case SYS_RENAME:      return sys_rename(a1, a2);
        case SYS_CHMOD:       return sys_chmod(a1, a2);
        case SYS_FSYNC:       return sys_fsync(a1);
        case SYS_SIGPROCMASK: return sys_sigprocmask(a1, a2, a3);
        case SYS_SIGPENDING:  return sys_sigpending(a1);
        case SYS_READV:       return sys_readv(a1, a2, a3);
        case SYS_WRITEV:      return sys_writev(a1, a2, a3);
        case SYS_GETRANDOM:   return sys_getrandom(a1, a2, a3);
        case SYS_REBOOT:      return sys_reboot();
        case SYS_SETHOSTNAME: return sys_sethostname(a1, a2);
        case SYS_GETHOSTNAME: return sys_gethostname(a1, a2);
        case SYS_UMASK:       return sys_umask(a1);
        case SYS_MKNOD:       return sys_mknod(a1, a2, a3);
        default:         return (uint64_t)-1;
    }
}

/* ── Init ─────────────────────────────────────────────────────── */

void syscall_init(void) {
    /* Initialize the compiler mutex */
    cc_mutex = mutex_init();

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
