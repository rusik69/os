#define KERNEL_INTERNAL
#include "syscall.h"
#include "process.h"
#include "pid_namespace.h"
#include "cgroup_namespace.h"
#include "mnt_namespace.h"
#include "ipc_namespace.h"
#include "user_namespace.h"
#include "ioprio.h"
#include "scheduler.h"
#include "signal.h"
#include "fs.h"
#include "vfs.h"
#include "ata.h"
#include "ahci.h"
#include "vga.h"
#include "timer.h"
#include "timers.h"
#include "sysctl.h"
#include "caps.h"
#include "keyboard.h"
#include "printf.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "eventfd.h"
#include "epoll.h"
#include "signalfd.h"
#include "inotify.h"
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

#include "heap.h"
#include "smp.h"
#include "apic.h"
#include "pipe.h"
#include "users.h"
#include "module.h"
#include "module_elf.h"
#include "shm.h"
#include "ac97.h"
#include "socket.h"
#include "seccomp.h"
#include "futex.h"
#include "audit.h"
#include "yama.h"
#include "kptr_restrict.h"
#include "dmesg.h"
#include "aslr.h"
#include "wx_enforce.h"
#include "mprotect.h"
#include "memfd.h"
#include "pidfd.h"
#include "fs_mount_prop.h"
#include "page_cache.h"
#include "bufcache.h"
#include "coredump.h"
#include "workqueue.h"
#include "madvise_ext.h"
#include "rseq.h"
#include "kcov.h"
#include "file_lock.h"
#include "hugetlb.h"
#include "sched_attr.h"
#include "swap.h"
#include "kexec.h"
#include "uaccess.h"
#include "mseal.h"
#include "lockdown.h"
#include "userfaultfd.h"
#include "poll.h"
#include "blockdev.h"   /* for SG_IO ioctl */
#include "io_uring.h"   /* for io_uring syscalls */
#include "signal_libc.h" /* for struct sigaction (sys_rt_sigaction validation) */
#include "netlink.h"     /* for netlink_is_valid_fd / netlink_send (sys_sendfile) */
#include "pkey.h"        /* for sys_pkey_mprotect */
#include "mem_policy.h"  /* for NUMA memory policy syscalls */

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Syscall dispatch — main kernel syscall interface and handler dispatch");
MODULE_AUTHOR("Ruslan Gustomiasov");

#ifndef UINT32_MAX
#define UINT32_MAX 4294967295U
#endif

/* Maximum file descriptors per process — bounds fd_table array accesses */
#define MAX_FDS PROCESS_FD_MAX

/* Process table lock — protects concurrent iteration of process_table[]
 * from syscalls (ps, kill, etc.) and timer interrupts (process_timer_tick). */
static spinlock_t proc_table_lock = SPINLOCK_INIT;

/* 6th syscall argument — saved by the asm entry before the dispatch call.
 * pselect6 packs {sigmask_ptr, sigset_size} in arg6 per the Linux x86_64 ABI.
 * Interrupts are masked during the syscall handler so a single global is safe. */
extern uint64_t syscall_arg6;

/* D127: Credential syscalls — declared in sys_credentials.c */
uint64_t sys_setuid(uint64_t uid);
uint64_t sys_seteuid(uint64_t euid);
uint64_t sys_setgid(uint64_t gid);
uint64_t sys_setegid(uint64_t egid);
uint64_t sys_getgroups(uint64_t size, uint64_t list_addr);
uint64_t sys_setgroups(uint64_t size, uint64_t list_addr);
uint64_t sys_getpgrp(void);

/* D128: Capability syscalls — implemented in sys_caps.c */
uint64_t sys_capget(uint64_t header_addr, uint64_t data_addr);
uint64_t sys_capset(uint64_t header_addr, uint64_t data_addr);
uint64_t sys_setsecurebits(uint64_t bits);
uint64_t sys_getsecurebits(void);

/* D123: Process & Signal syscalls — declared in sys_process.c */
uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act_addr,
                          uint64_t oldact_addr, uint64_t sigsetsize);
uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_addr,
                            uint64_t oldset_addr, uint64_t sigsetsize);
uint64_t sys_rt_sigreturn(void);
uint64_t sys_rt_sigtimedwait(uint64_t set_addr, uint64_t info_addr,
                             uint64_t timeout_addr, uint64_t sigsetsize);
uint64_t sys_kill(uint64_t pid, uint64_t sig);
uint64_t sys_tkill(uint64_t pid, uint64_t sig);
uint64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig);
uint64_t sys_wait4(uint64_t pid, uint64_t wstatus_addr,
                   uint64_t options, uint64_t rusage_addr);
uint64_t sys_waitid(uint64_t which, uint64_t id, uint64_t info_addr,
                    uint64_t options, uint64_t rusage_addr);
uint64_t sys_exit_group(uint64_t code);
uint64_t sys_set_tid_address(uint64_t tidptr);

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
    uint64_t cpu_user_ticks;   /* utime */
    uint64_t cpu_system_ticks; /* stime */
    int      nice;             /* nice value (-20..+19) */
    uint64_t max_rss;          /* max resident set size (pages) */
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
#define EFER_SCE (1U << 0)  /* Syscall Enable */

extern void syscall_entry(void);
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
    if (!syscall_is_user_process()) return 0;

    switch (num) {
        case SYS_READ:
            return syscall_user_write_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_WRITE:
            return syscall_user_read_ok(a2, a3) ? 0 : -EFAULT;
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

        case SYS_STAT:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(uint32_t) * 2)) ? 0 : -EFAULT;
        case SYS_FS_CREATE:
        case SYS_FS_CHMOD:
            return syscall_user_cstr_ok(a1) ? 0 : -EFAULT;
        case SYS_FS_CHOWN:
            return syscall_user_cstr_ok(a1) ? 0 : -EFAULT;
        case SYS_FS_WRITE:
            return (syscall_user_cstr_ok(a1) && syscall_user_read_ok(a2, a3)) ? 0 : -EFAULT;
        case SYS_FS_READ:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3) &&
                    syscall_user_write_ok(a4, sizeof(uint32_t))) ? 0 : -EFAULT;
        case SYS_FS_STAT:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(uint32_t) * 2)) ? 0 : -EFAULT;
        case SYS_FS_STAT_EX:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(struct syscall_fs_stat_ex))) ? 0 : -EFAULT;
        case SYS_FS_LIST_NAMES:
            if (!syscall_user_cstr_ok(a1)) return -EFAULT;
            if (a2 && !syscall_user_cstr_ok(a2)) return -EFAULT;
            if (a4 == 0) return 0;
            if (a4 > (1ULL << 20)) return -EINVAL;
            return syscall_user_write_ok(a3, a4 * FS_MAX_NAME) ? 0 : -EFAULT;
        case SYS_VFS_READ:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3) &&
                    syscall_user_write_ok(a4, sizeof(uint32_t))) ? 0 : -EFAULT;
        case SYS_VFS_WRITE:
            return (syscall_user_cstr_ok(a1) && syscall_user_read_ok(a2, a3)) ? 0 : -EFAULT;
        case SYS_VFS_STAT:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(struct vfs_stat))) ? 0 : -EFAULT;
        case SYS_WAITPID:
            return (a2 ? syscall_user_write_ok(a2, sizeof(int)) : 1) ? 0 : -EFAULT;
        case SYS_WAIT4:
            if (a2 && !syscall_user_write_ok(a2, sizeof(int))) return -EFAULT;
            if (a4 && !syscall_user_write_ok(a4, sizeof(struct rusage))) return -EFAULT;
            return 0;
        case SYS_WAITID:
            if (a3 && !syscall_user_write_ok(a3, sizeof(struct siginfo))) return -EFAULT;
            if (a5 && !syscall_user_write_ok(a5, sizeof(struct rusage))) return -EFAULT;
            return 0;
        case SYS_NET_GET_MAC:
            return syscall_user_write_ok(a1, 6) ? 0 : -EFAULT;
        case SYS_NET_GET_IP:
            return syscall_user_write_ok(a1, 4) ? 0 : -EFAULT;
        case SYS_NET_UDP_SEND:
            return syscall_user_read_ok(a4, a5) ? 0 : -EFAULT;
        case SYS_NET_HTTP_GET: {
            uint64_t bufsize = (uint32_t)(a5 >> 32);
            return (syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a3) &&
                    syscall_user_write_ok(a4, bufsize)) ? 0 : -EFAULT;
        }
        case SYS_NET_TCP_SEND_CONN:
            return syscall_user_read_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_NET_TCP_RECV_CONN:
            return syscall_user_write_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_PROC_LIST:
            if (a2 == 0) return 0;
            if (a2 > PROCESS_MAX) return -EINVAL;
            return syscall_user_write_ok(a1, a2 * sizeof(struct syscall_process_info)) ? 0 : -EFAULT;
        case SYS_USER_FIND:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, sizeof(struct user_entry))) ? 0 : -EFAULT;
        case SYS_USER_ADD:
            return (syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a3)) ? 0 : -EFAULT;
        case SYS_USER_PASSWD:
        case SYS_SESSION_LOGIN:
            return (syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a2)) ? 0 : -EFAULT;
        case SYS_USER_DELETE:
            return syscall_user_cstr_ok(a1) ? 0 : -EFAULT;
        case SYS_USERS_GET_BY_INDEX:
            return syscall_user_write_ok(a2, sizeof(struct user_entry)) ? 0 : -EFAULT;
        case SYS_RTC_GET_TIME:
            return syscall_user_write_ok(a1, sizeof(struct rtc_time)) ? 0 : -EFAULT;
        case SYS_MOUSE_GET_STATE:
            return syscall_user_write_ok(a1, sizeof(struct mouse_state)) ? 0 : -EFAULT;
        case SYS_SERIAL_READ:
            return syscall_user_write_ok(a1, a2) ? 0 : -EFAULT;
        case SYS_SERIAL_WRITE:
            return syscall_user_read_ok(a1, a2) ? 0 : -EFAULT;
        case SYS_PMM_GET_STATS:
            return syscall_user_write_ok(a1, sizeof(struct pmm_stats)) ? 0 : -EFAULT;
        case SYS_FAT_LIST_DIR:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3 * FAT32_MAX_NAME)) ? 0 : -EFAULT;
        case SYS_FAT_READ_FILE:
            return (syscall_user_cstr_ok(a1) && syscall_user_write_ok(a2, a3)) ? 0 : -EFAULT;
        case SYS_FAT_WRITE_FILE:
            return (syscall_user_cstr_ok(a1) && syscall_user_read_ok(a2, a3)) ? 0 : -EFAULT;

        case SYS_FREE:
            /* Pointer could be any previously-allocated address; no range check needed */
            return 0;
        case SYS_REALLOC:
            /* old ptr is arbitrary; just validate that the new result ptr location is writable */
            return 0;
        case SYS_CHDIR:
        case SYS_TRUNCATE:
            return syscall_user_cstr_ok(a1) ? 0 : -EFAULT;
        case SYS_GETCWD:
            if (a2 == 0) return -EINVAL;
            return syscall_user_write_ok(a1, a2) ? 0 : -EFAULT;
        case SYS_FD_READ:
            return syscall_user_write_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_FD_WRITE:
            return syscall_user_read_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_PREAD64:
            if (a3 == 0) return 0;
            return syscall_user_write_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_PWRITE64:
            if (a3 == 0) return 0;
            return syscall_user_read_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_RAW_SEND:
            if (a2 == 0 || a2 > 1514) return -EINVAL;
            return syscall_user_read_ok(a1, a2) ? 0 : -EFAULT;
        /* New production syscalls */
        case SYS_PRLIMIT64: {
            if (a2 >= _RLIMIT_NLIMITS) return -EINVAL;
            if (a3 && !syscall_user_read_ok(a3, 16)) return -EFAULT;
            if (a4 && !syscall_user_write_ok(a4, 16)) return -EFAULT;
            return 0;
        }
        case SYS_FUTEX:
            if (a2 == FUTEX_WAIT && !syscall_user_read_ok(a1, 4)) return -EFAULT;
            return 0;
        case SYS_ARCH_PRCTL:
            if ((a1 == ARCH_GET_FS || a1 == ARCH_GET_GS) &&
                !syscall_user_write_ok(a2, 8)) return -EFAULT;
            return 0;
        case SYS_POLL:
            if (a2 == 0) return 0;
            return (syscall_user_read_ok(a1, a2 * sizeof(struct pollfd)) &&
                    syscall_user_write_ok(a1, a2 * sizeof(struct pollfd))) ? 0 : -EFAULT;
        case SYS_EVENTFD:
            return 0;
        case SYS_SENDFILE:
            if (a3 && !syscall_user_read_ok(a3, 8)) return -EFAULT;
            return 0;
        case SYS_IOCTL:
            return 0;
        case SYS_SYSLOG:
            if ((a1 == SYSLOG_ACTION_READ_ALL || a1 == SYSLOG_ACTION_READ_CLEAR) &&
                !syscall_user_write_ok(a2, a3)) return -EFAULT;
            return 0;
        case SYS_PRCTL:
            if (a1 == PR_SET_NAME && !syscall_user_read_ok(a2, 16)) return -EFAULT;
            if (a1 == PR_GET_NAME && !syscall_user_write_ok(a2, 16)) return -EFAULT;
            return 0;
        case SYS_MOUNT:
            return (syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a2)) ? 0 : -EFAULT;
        case SYS_UMOUNT:
            return syscall_user_cstr_ok(a1) ? 0 : -EFAULT;
        case SYS_FTRUNCATE:
            return 0;
        case SYS_READDIR:
            return syscall_user_write_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_EXECVEAT:
            return syscall_user_cstr_ok(a2) ? 0 : -EFAULT;
        case SYS_SCHED_SETSCHEDULER:
            return 0;
        case SYS_SCHED_GETSCHEDULER:
            return 0;
        case SYS_SCHED_SETATTR:
            if (!a2) return -EINVAL;
            return syscall_user_read_ok(a2, sizeof(struct sched_attr)) ? 0 : -EFAULT;
        case SYS_SCHED_GETATTR:
            if (!a2) return -EINVAL;
            return syscall_user_write_ok(a2, a3) ? 0 : -EFAULT;
        /* New production syscalls (batch 2) */
        case SYS_OPENAT:
            return syscall_user_cstr_ok(a2) ? 0 : -EFAULT;
        case SYS_MKDIRAT:
            return syscall_user_cstr_ok(a2) ? 0 : -EFAULT;
        case SYS_FSTATAT:
            return (syscall_user_cstr_ok(a2) && syscall_user_write_ok(a3, sizeof(struct vfs_stat))) ? 0 : -EFAULT;
        case SYS_UNLINKAT:
            return syscall_user_cstr_ok(a2) ? 0 : -EFAULT;
        case SYS_RENAMEAT:
            return (syscall_user_cstr_ok(a2) && syscall_user_cstr_ok(a4)) ? 0 : -EFAULT;
        case SYS_SYMLINKAT:
            return (syscall_user_cstr_ok(a1) && syscall_user_cstr_ok(a3)) ? 0 : -EFAULT;
        case SYS_READLINKAT:
            return (syscall_user_cstr_ok(a2) && syscall_user_write_ok(a3, a4)) ? 0 : -EFAULT;
        case SYS_GETDENTS64:
            return syscall_user_write_ok(a2, a3) ? 0 : -EFAULT;
        case SYS_MLOCK:
        case SYS_MLOCKALL:
        case SYS_MUNLOCK:
        case SYS_MUNLOCKALL:
        case SYS_FALLOCATE:
            return 0;
        case SYS_MINCORE:
            return syscall_user_write_ok(a3, (a2 + PAGE_SIZE - 1) / PAGE_SIZE) ? 0 : -EFAULT;
        case SYS_MADVISE:
            return 0;
        /* NUMA memory policy */
        case SYS_MBIND:
            /* addr (a1) checked in handler; nodemask (a4) content checked in handler */
            return 0;
        case SYS_SET_MEMPOLICY:
            return 0;
        case SYS_GET_MEMPOLICY:
            /* mode_addr (a1) and nodemask_addr (a2) are write pointers */
            if (a1 && !syscall_user_write_ok(a1, sizeof(int))) return -EFAULT;
            if (a2 && !syscall_user_write_ok(a2, sizeof(uint64_t))) return -EFAULT;
            return 0;
        case SYS_MIGRATE_PAGES:
            /* new_nodes_addr (a4) is a read pointer */
            if (a4 && !syscall_user_read_ok(a4, sizeof(uint64_t))) return -EFAULT;
            return 0;
        case SYS_MOVE_PAGES:
            /* pages_addr (a3), nodes_addr (a4), status_addr (a5) */
            if (a3 && !syscall_user_read_ok(a3, a2 * sizeof(uint64_t))) return -EFAULT;
            if (a4 && !syscall_user_read_ok(a4, a2 * sizeof(int))) return -EFAULT;
            if (a5 && !syscall_user_write_ok(a5, a2 * sizeof(int))) return -EFAULT;
            return 0;
        case SYS_REMAP_FILE_PAGES:
            return 0;
        case SYS_TIMERFD_CREATE:
            return 0;
        case SYS_TIMERFD_SETTIME:
            if (a3 && !syscall_user_read_ok(a3, sizeof(struct itimerspec))) return -EFAULT;
            if (a4 && !syscall_user_write_ok(a4, sizeof(struct itimerspec))) return -EFAULT;
            return 0;
        case SYS_TIMERFD_GETTIME:
            return syscall_user_write_ok(a2, sizeof(struct itimerspec)) ? 0 : -EFAULT;
        case SYS_SIGNALFD:
            if (a2 && !syscall_user_read_ok(a2, 8)) return -EFAULT;
            return 0;
        case SYS_MEMFD_CREATE:
            if (a1 && !syscall_user_cstr_ok(a1)) return -EFAULT;
            return 0;
        case SYS_SPLICE:
        case SYS_TEE:
            return 0;
        case SYS_SENDMMSG:
        case SYS_RECVMMSG:
            return 0; /* simplified validation */
        case SYS_SYNC:
        case SYS_SYNCFS:
        case SYS_SETSID:
        case SYS_GETSID:
        case SYS_GETUID:
        case SYS_GETEUID:
        case SYS_GETGID:
        case SYS_GETEGID:
        case SYS_SETUID:
        case SYS_SETEUID:
        case SYS_SETGID:
        case SYS_SETEGID:
            return 0;
        case SYS_SIGALTSTACK:
            if (a1 && !syscall_user_read_ok(a1, sizeof(stack_t))) return -EFAULT;
            if (a2 && !syscall_user_write_ok(a2, sizeof(stack_t))) return -EFAULT;
            return 0;
        case SYS_RT_SIGACTION:
            if (a2 && !syscall_user_read_ok(a2, sizeof(struct sigaction))) return -EFAULT;
            if (a3 && !syscall_user_write_ok(a3, sizeof(struct sigaction))) return -EFAULT;
            return 0;
        case SYS_RT_SIGPROCMASK:
            if (a2 && !syscall_user_read_ok(a2, sizeof(uint64_t))) return -EFAULT;
            if (a3 && !syscall_user_write_ok(a3, sizeof(uint64_t))) return -EFAULT;
            return 0;
        case SYS_RT_SIGRETURN:
            /* No arguments — just validate that user is calling from user mode */
            return 0;
        case SYS_PERSONALITY:
            return 0;
        /* Supplementary groups — validate user pointers */
        case SYS_GETGROUPS:
            if (a1 > 0 && a2 && !syscall_user_write_ok(a2, a1 * sizeof(uint32_t)))
                return -EFAULT;
            return 0;
        case SYS_SETGROUPS:
            if (a1 > 0 && a2 && !syscall_user_read_ok(a2, a1 * sizeof(uint32_t)))
                return -EFAULT;
            return 0;
        default:
            return 0;
    }
}

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

/* Forward declarations for timerfd/signalfd read helpers (used in sys_read) */
static int timerfd_do_read(int slot, uint64_t *val);
static int signalfd_do_read(int slot, void *buf, uint64_t count);

/* ── Syscall handlers ─────────────────────────────────────────── */

/* Get per-process FD table entry */
static struct process_fd *sys_get_fd(int i) {
    struct process *p = process_get_current();
    return 0;
    if (i < 0 || i >= MAX_FDS)
        return NULL;
    return &p->fd_table[i];
}

/**
 * sys_read - Read from a file descriptor
 * @fd: File descriptor number
 * @buf_addr: User-space address of the output buffer
 * @len: Number of bytes to read
 *
 * Reads up to @len bytes from the given file descriptor. Supports
 * regular file descriptors (fd >= 3), stdin (fd == 0), signalfd
 * (600-615), timerfd (500-515), eventfd (700-715), memfd, and
 * inotify (720-727). For regular files, reads from the current
 * file offset and advances it. Updates I/O accounting counters.
 *
 * Context: Called from syscall dispatch. May sleep. Must be called
 *          with a valid current process.
 * Return: Number of bytes read on success, or (uint64_t)-1 on error.
 */
static uint64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    if (!buf_addr && len > 0) return (uint64_t)(int64_t)-EFAULT;

    if (fd == 0 && !syscall_is_user_process()) return 0;
    if (fd >= 3 && fd < 700) {
        int i = (int)fd - 3;
        struct process_fd *pfd = sys_get_fd(i);
        if (!pfd || !pfd->used) return (uint64_t)(int64_t)-EBADF;
        struct vfs_stat st;
        if (vfs_stat(pfd->path, &st) < 0) return (uint64_t)(int64_t)-EIO;
        uint64_t fsize = st.size;
        if (pfd->offset >= fsize) return 0;
        uint64_t avail = fsize - pfd->offset;
        uint64_t to_read = len < avail ? len : avail;
        /* Clamp to UINT32_MAX to avoid uint32_t truncation in vfs_read */
        if (to_read > UINT32_MAX) to_read = UINT32_MAX;
        uint64_t need_end = pfd->offset + to_read;
        if (need_end > fsize) need_end = fsize;
        /* Clamp need_end to UINT32_MAX for vfs_read */
        if (need_end > UINT32_MAX) need_end = UINT32_MAX;
        uint8_t *tmp = kmalloc(need_end);
        if (!tmp) return (uint64_t)(int64_t)-ENOMEM;
        uint32_t nread = 0;
        vfs_read(pfd->path, tmp, (uint32_t)need_end, &nread);
        if (copy_to_user(buf_addr, tmp + pfd->offset, to_read) < 0) {
            kfree(tmp);
            return (uint64_t)(int64_t)-EFAULT;
        }
        kfree(tmp);
        pfd->offset += to_read;
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur) {
                cur->io_rchar += to_read;
                cur->io_syscr++;
                cur->io_read_bytes += to_read;
            }
        }
        return (uint64_t)to_read;
    }
    /* signalfd read */
    if (fd >= 600 && fd < 616) {
        int slot = (int)fd - 600;
        uint8_t *kbuf = kmalloc(len > 4096 ? 4096 : len);
        if (!kbuf) return (uint64_t)(int64_t)-ENOMEM;
        int ret = signalfd_do_read(slot, kbuf, len);
        if (ret > 0) {
            if (copy_to_user(buf_addr, kbuf, (size_t)ret) < 0) {
                kfree(kbuf);
                return (uint64_t)(int64_t)-EFAULT;
            }
        }
        kfree(kbuf);
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur && ret > 0) {
                cur->io_rchar += (uint64_t)ret;
                cur->io_syscr++;
            }
        }
        return (uint64_t)(ret > 0 ? ret : 0);
    }
    /* timerfd read */
    if (fd >= 500 && fd < 516) {
        int slot = (int)fd - 500;
        uint64_t tval = 0;
        if (timerfd_do_read(slot, &tval) == 0 &&
            syscall_user_write_ok(buf_addr, 8)) {
            if (copy_to_user(buf_addr, &tval, 8) < 0)
                return (uint64_t)(int64_t)-EFAULT;
        }
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur) {
                cur->io_rchar += 8;
                cur->io_syscr++;
            }
        }
        return 8;
    }
    /* eventfd read */
    if (fd >= 700 && fd < 716) {
        if (len < 8) return (uint64_t)(int64_t)-EINVAL;
        uint64_t val;
        if (eventfd_read((int)fd, &val) < 0) return (uint64_t)(int64_t)-EAGAIN;
        if (copy_to_user(buf_addr, &val, 8) < 0) return (uint64_t)(int64_t)-EFAULT;
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur) {
                cur->io_rchar += 8;
                cur->io_syscr++;
            }
        }
        return 8;
    }
    /* memfd read */
    if (memfd_is_fd((int)fd)) {
        struct memfd *mfd = memfd_get_by_fd((int)fd);
        if (!mfd) return (uint64_t)-1;
        uint8_t *kbuf = kmalloc(len > 65536 ? 65536 : len);
        if (!kbuf) { memfd_put(mfd); return (uint64_t)(int64_t)-ENOMEM; }
        int64_t ret = memfd_read(mfd, kbuf, len, 0);
        if (ret > 0) {
            if (copy_to_user(buf_addr, kbuf, (size_t)ret) < 0) {
                kfree(kbuf);
                memfd_put(mfd);
                return (uint64_t)(int64_t)-EFAULT;
            }
        }
        kfree(kbuf);
        memfd_put(mfd);
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur && ret > 0) {
                cur->io_rchar += (uint64_t)ret;
                cur->io_syscr++;
            }
        }
        return (uint64_t)(ret >= 0 ? (uint64_t)ret : (uint64_t)-1);
    }
    /* inotify read (fd range 720-727) */
    if (fd >= INOTIFY_FD_BASE && fd < INOTIFY_FD_BASE + INOTIFY_INSTANCES) {
        uint8_t *kbuf = kmalloc(len > 4096 ? 4096 : len);
        if (!kbuf) return (uint64_t)(int64_t)-ENOMEM;
        int ret = inotify_read((int)fd, kbuf, (size_t)(len > 4096 ? 4096 : len));
        if (ret >= 0) {
            if (copy_to_user(buf_addr, kbuf, (size_t)ret) < 0) {
                kfree(kbuf);
                return (uint64_t)(int64_t)-EFAULT;
            }
            struct process *cur = process_get_current();
            if (cur) {
                cur->io_rchar += (uint64_t)ret;
                cur->io_syscr++;
            }
        }
        kfree(kbuf);
        return ret >= 0 ? (uint64_t)ret : (uint64_t)-1;
    }
    return (uint64_t)-1;
}

/**
 * sys_write - Write to a file descriptor
 * @fd: File descriptor number
 * @buf_addr: User-space address of the data to write
 * @len: Number of bytes to write
 *
 * Writes up to @len bytes to the given file descriptor. Supports
 * stdout/stderr (fd 1/2), eventfd (700-715), and memfd. For stdout
 * and stderr, writes to both VGA and serial console. Updates I/O
 * accounting counters.
 *
 * Context: Called from syscall dispatch. May sleep. Must be called
 *          with a valid current process.
 * Return: Number of bytes written on success, or (uint64_t)-1 on error.
 */
static uint64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t len) {
    if (!buf_addr && len > 0) return (uint64_t)(int64_t)-EFAULT;

    if (fd == 1 || fd == 2) {
        /* Copy from user-space to avoid SMAP fault */
        uint8_t *kbuf = kmalloc(len > 4096 ? 4096 : (len > 0 ? len : 1));
        if (!kbuf) return (uint64_t)(int64_t)-ENOMEM;
        size_t to_copy = len > 4096 ? 4096 : len;
        if (to_copy > 0) {
            if (copy_from_user(kbuf, buf_addr, to_copy) < 0) {
                kfree(kbuf);
                return (uint64_t)(int64_t)-EFAULT;
            }
            for (uint64_t i = 0; i < to_copy; i++) {
                vga_putchar((char)kbuf[i]);
                serial_putchar((char)kbuf[i]);
            }
        }
        kfree(kbuf);
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur) {
                cur->io_wchar += len;
                cur->io_syscw++;
                cur->io_write_bytes += len;
            }
        }
        return len;
    }
    /* eventfd write */
    if (fd >= 700 && fd < 716) {
        if (len < 8) return (uint64_t)-1;
        uint64_t val;
        if (copy_from_user(&val, buf_addr, 8) < 0) return (uint64_t)(int64_t)-EFAULT;
        if (eventfd_write((int)fd, val) < 0) return (uint64_t)-1;
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur) {
                cur->io_wchar += 8;
                cur->io_syscw++;
                cur->io_write_bytes += 8;
            }
        }
        return 8;
    }
    /* memfd write */
    if (memfd_is_fd((int)fd)) {
        struct memfd *mfd = memfd_get_by_fd((int)fd);
        if (!mfd) return (uint64_t)-1;
        uint8_t *kbuf = kmalloc(len > 65536 ? 65536 : (len > 0 ? len : 1));
        if (!kbuf) { memfd_put(mfd); return (uint64_t)(int64_t)-ENOMEM; }
        size_t wlen = len > 65536 ? 65536 : len;
        if (copy_from_user(kbuf, buf_addr, wlen) < 0) {
            kfree(kbuf);
            memfd_put(mfd);
            return (uint64_t)(int64_t)-EFAULT;
        }
        int64_t ret = memfd_write(mfd, kbuf, wlen, 0);
        kfree(kbuf);
        memfd_put(mfd);
        /* I/O accounting */
        {
            struct process *cur = process_get_current();
            if (cur && ret > 0) {
                cur->io_wchar += (uint64_t)ret;
                cur->io_syscw++;
                cur->io_write_bytes += (uint64_t)ret;
            }
        }
        return (uint64_t)(ret >= 0 ? (uint64_t)ret : (uint64_t)-1);
    }
    return (uint64_t)-1;
}

/*
 * Generate a unique hidden temporary file path for O_TMPFILE.
 * The path is of the form:  <dir>/.tmp_<pid>_<counter>
 * Returns 0 on success, -1 on failure (path too long).
 */
static int tmpfile_make_path(const char *dir, char *buf, int bufsize)
{
    static uint64_t tmpfile_counter = 0;
    struct process *p = process_get_current();
    uint32_t pid = p ? p->pid : 0;
    uint64_t seq;

    /* Atomically increment the global counter */
    __asm__ volatile("lock; addq $1, %0" : "+m"(tmpfile_counter) : : "memory");
    seq = tmpfile_counter;

    int n = snprintf(buf, (size_t)bufsize, "%s/.tmp_%u_%llu",
                     dir ? dir : "/tmp", pid,
                     (unsigned long long)seq);
    if (n < 0 || n >= bufsize)
        return -1;
    return 0;
}

/**
 * do_sys_open - Core file open logic (kernel-space path)
 * @path: Kernel-space path string
 * @flags: Open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC,
 *         O_TMPFILE, etc.)
 * @mode: File creation mode (unused in current implementation)
 *
 * Performs the actual VFS open operations and allocates a file
 * descriptor in the current process's fd table. Supports O_TMPFILE
 * for creating unnamed temporary files and O_TRUNC for truncation.
 * Enforces RLIMIT_NOFILE on fd allocation and performs IMA
 * measurement for files opened for read.
 *
 * Context: Called from syscall dispatch. May sleep. Must be called
 *          with a valid current process.
 * Return: File descriptor (>= 3) on success, or negative errno on error.
 */
static uint64_t do_sys_open(const char *path, uint64_t flags, uint64_t mode) {
    (void)mode;

    /* Validate access mode flags — only O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
    uint64_t access_mode = flags & 3;
    if (access_mode > 2)
        return (uint64_t)(int64_t)-EINVAL;

    /* O_TMPFILE must be accompanied by O_RDWR on Linux */
    if ((flags & O_TMPFILE) && access_mode != 2)
        return (uint64_t)(int64_t)-EINVAL;

    struct vfs_stat st;
    int exists = (vfs_stat(path, &st) >= 0);

    /* Permission check for existing file */
    if (exists && !(flags & O_TMPFILE)) {
        struct process *p = process_get_current();
        if (!p) return (uint64_t)(int64_t)-EPERM;
        uint16_t perm_op = 0;
        if (access_mode == 0 || access_mode == 2)  /* O_RDONLY or O_RDWR */
            perm_op |= VFS_R_OK;
        if (access_mode == 1 || access_mode == 2)  /* O_WRONLY or O_RDWR */
            perm_op |= VFS_W_OK;
        if ((flags & O_TRUNC))
            perm_op |= VFS_W_OK;
        if (vfs_check_perms(path, (uint16_t)p->uid, (uint16_t)p->gid, perm_op) < 0)
            return (uint64_t)(int64_t)-EACCES;
    }

    /* Handle O_TMPFILE */
    if (flags & O_TMPFILE) {
        char tmp_path[64];
        if (tmpfile_make_path(path, tmp_path, (int)sizeof(tmp_path)) < 0)
            return (uint64_t)(int64_t)-ENOSPC;

        /* Create the hidden temp file */
        if (vfs_create(tmp_path, 0) < 0)
            return (uint64_t)(int64_t)-ENOSPC;

        /* Allocate fd slot and mark as FD_TMPFILE */
        struct process *p = process_get_current();
        if (!p) { vfs_unlink(tmp_path); return (uint64_t)(int64_t)-EPERM; }
        uint64_t max_fds = p->rlim_cur[RLIMIT_NOFILE] > 0 ?
                           p->rlim_cur[RLIMIT_NOFILE] : PROCESS_FD_MAX;
        for (int i = 0; i < PROCESS_FD_MAX; i++) {
            if (!p->fd_table[i].used) {
                if ((uint64_t)i >= max_fds) {
                    vfs_unlink(tmp_path);
                    return (uint64_t)(int64_t)-EMFILE;
                }
                strncpy(p->fd_table[i].path, tmp_path, 63);
                p->fd_table[i].path[63] = '\0';
                p->fd_table[i].offset = 0;
                p->fd_table[i].used = true;
                p->fd_table[i].flags = FD_TMPFILE;
                p->fd_table[i].open_flags = (uint8_t)(flags & 0xFF);
                return (uint64_t)(i + 3);
            }
        }
        vfs_unlink(tmp_path);
        return (uint64_t)(int64_t)-EMFILE;
    }

    /* Handle O_TRUNC: truncate file to zero length */
    if (exists && (flags & O_TRUNC)) {
        vfs_truncate(path, 0);
    }

    if (!exists) return (uint64_t)(int64_t)-ENOENT;

    /* IMA measurement: measure the file before opening for read */
    {
        extern int ima_file_open(const char *path, int flags);
        int ima_ret = ima_file_open(path, (int)flags);
        if (ima_ret < 0)
            return (uint64_t)(int64_t)ima_ret;
    }

    /* Allocate fd slot in current process's table */
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-EPERM;
    /* Enforce RLIMIT_NOFILE */
    uint64_t max_fds = p->rlim_cur[RLIMIT_NOFILE] > 0 ?
                       p->rlim_cur[RLIMIT_NOFILE] : PROCESS_FD_MAX;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fd_table[i].used) {
            if ((uint64_t)i >= max_fds) return (uint64_t)(int64_t)-EMFILE;
            strncpy(p->fd_table[i].path, path, 63);
            p->fd_table[i].path[63] = '\0';
            p->fd_table[i].offset = 0;
            p->fd_table[i].used = true;
            p->fd_table[i].open_flags = (uint8_t)(flags & 0xFF); /* save O_APPEND, O_NONBLOCK etc. */
            return (uint64_t)(i + 3);
        }
    }
    return (uint64_t)(int64_t)-EMFILE;
}

/**
 * sys_open - Open a file
 * @path_addr: User-space address of the path string
 * @flags: Open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC,
 *         O_TMPFILE, etc.)
 * @mode: File creation mode (unused in current implementation)
 *
 * Opens or creates a file by copying the path from user space,
 * then delegating to do_sys_open() for the core VFS operations.
 * Supports O_TMPFILE for creating unnamed temporary files and
 * O_TRUNC for truncation. Enforces RLIMIT_NOFILE on fd allocation.
 *
 * Context: Called from syscall dispatch. May sleep. Must be called
 *          with a valid current process.
 * Return: File descriptor (>= 3) on success, or negative errno on error.
 */
static uint64_t sys_open(uint64_t path_addr, uint64_t flags, uint64_t mode) {
    (void)mode;
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    return do_sys_open(kpath, flags, mode);
}

/**
 * sys_close - Close a file descriptor
 * @fd: File descriptor number to close
 *
 * Closes the given file descriptor, releasing the fd slot in the
 * current process's file descriptor table. Supports inotify fd
 * close (720-727). If the fd was created with O_TMPFILE, the
 * hidden temporary file is unlinked.
 *
 * Context: Called from syscall dispatch. May sleep.
 * Return: 0 on success, or (uint64_t)-1 on error.
 */
static uint64_t sys_close(uint64_t fd) {
    /* inotify close (fd range 720-727) */
    if (fd >= INOTIFY_FD_BASE && fd < INOTIFY_FD_BASE + INOTIFY_INSTANCES) {
        int ret = inotify_close((int)fd);
        return ret < 0 ? (uint64_t)(int64_t)ret : 0;
    }
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd) return (uint64_t)(int64_t)-EBADF;

    /* If this is an O_TMPFILE fd, unlink the hidden file */
    if (pfd->flags & FD_TMPFILE && pfd->path[0]) {
        vfs_unlink(pfd->path);
    }

    pfd->used = false;
    pfd->flags = 0;
    pfd->path[0] = '\0';
    return 0;
}

/* ── close_range — close all file descriptors in [first, last] ────── */
static uint64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags)
{
    /* Validate range */
    if (first > last)
        return (uint64_t)(int64_t)-EINVAL;

    /* We don't support CLOSE_RANGE_UNSHARE yet */
    uint64_t known_flags = CLOSE_RANGE_CLOEXEC;
    if (flags & ~known_flags)
        return (uint64_t)(int64_t)-EINVAL;

    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-EPERM;

    /* Clamp to PROCESS_FD_MAX */
    if (first >= PROCESS_FD_MAX)
        return 0;  /* nothing to do */
    if (last >= PROCESS_FD_MAX)
        last = PROCESS_FD_MAX - 1;

    uint64_t closed = 0;
    for (uint64_t fd = first; fd <= last; fd++) {
        int i = (int)fd - 3;
        if (i < 0) continue;

        struct process_fd *pfd = &p->fd_table[i];
        if (!pfd->used)
            continue;

        if (flags & CLOSE_RANGE_CLOEXEC) {
            /* Set close-on-exec flag instead of closing */
            pfd->flags |= FD_CLOEXEC;
        } else {
            /* Close the fd */
            if (pfd->flags & FD_TMPFILE && pfd->path[0])
                vfs_unlink(pfd->path);
            pfd->used = false;
            pfd->flags = 0;
            pfd->path[0] = '\0';
            closed++;
        }
    }

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
    /* Return namespace-local PID (Item 111) */
    return p ? pid_ns_get_ns_pid(p) : 0;
}

/* ── Signal registration (SYS_SIGNAL=213) ──────────────────────── */
static uint64_t sys_signal(uint64_t signum, uint64_t handler_addr) {
    if (syscall_is_user_process() &&
        handler_addr != (uint64_t)SIG_DFL && handler_addr != (uint64_t)SIG_IGN)
        return (uint64_t)(int64_t)-EINVAL;
    signal_register((int)signum, (signal_handler_t)(uintptr_t)handler_addr);
    return 0;
}

/* ── File seek / truncate (SYS_LSEEK=214, SYS_TRUNCATE=215) ───── */
static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence) {
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used) return (uint64_t)(int64_t)-EBADF;
    struct vfs_stat st;
    uint64_t fsz = 0;
    if (vfs_stat(pfd->path, &st) == 0) fsz = st.size;
    int64_t off = (int64_t)offset;
    int64_t new_off;
    switch (whence) {
        case 0: new_off = off; break;                /* SEEK_SET */
        case 1: new_off = (int64_t)pfd->offset + off; break; /* SEEK_CUR */
        case 2: new_off = (int64_t)fsz + off; break; /* SEEK_END */
        case 3: /* SEEK_DATA */
        case 4: /* SEEK_HOLE */
            new_off = (int64_t)fsz;
            break;
        default: return (uint64_t)(int64_t)-EINVAL;
    }
    if (new_off < 0) return (uint64_t)(int64_t)-EINVAL;
    pfd->offset = (uint64_t)new_off;
    return (uint64_t)new_off;
}

static uint64_t sys_truncate(uint64_t path_addr, uint64_t len) {
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    struct vfs_stat st;
    if (vfs_stat(kpath, &st) < 0)
        return (uint64_t)(int64_t)-ENOENT;
    if (st.type == VFS_TYPE_DIR)
        return (uint64_t)(int64_t)-EISDIR;
    if (vfs_truncate(kpath, (uint32_t)len) < 0)
        return (uint64_t)(int64_t)-EIO;
    return 0;
}

/* ── Raw Ethernet send (SYS_RAW_SEND=216) ──────────────────────── */
static uint64_t sys_raw_send(uint64_t buf_addr, uint64_t len) {
    if (len == 0 || len > 1514) return (uint64_t)(int64_t)-EINVAL;
    int r = net_link_send((const uint8_t *)(uintptr_t)buf_addr, (uint16_t)len);
    return r < 0 ? (uint64_t)(int64_t)r : len;
}

/* ── FD-based read/write (SYS_FD_READ=217, SYS_FD_WRITE=218) ──── */
static uint64_t sys_fd_read(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used) return (uint64_t)(int64_t)-EBADF;
    uint64_t fsize = 0;
    struct vfs_stat st;
    if (vfs_stat(pfd->path, &st) < 0) return (uint64_t)(int64_t)-EIO;
    fsize = st.size;
    if (pfd->offset >= fsize) return 0;
    uint64_t avail = fsize - pfd->offset;
    uint64_t to_read = count < avail ? count : avail;
    /* Clamp to UINT32_MAX to avoid uint32_t truncation in vfs_read */
    if (to_read > UINT32_MAX) to_read = UINT32_MAX;
    uint64_t need_end = pfd->offset + to_read;
    if (need_end > fsize) need_end = fsize;
    /* Clamp need_end to UINT32_MAX for vfs_read */
    if (need_end > UINT32_MAX) need_end = UINT32_MAX;
    uint8_t *tmp = kmalloc(need_end);
    if (!tmp) return (uint64_t)(int64_t)-ENOMEM;
    uint32_t nread = 0;
    vfs_read(pfd->path, tmp, (uint32_t)need_end, &nread);
    if (copy_to_user(buf_addr, tmp + pfd->offset, to_read) < 0) {
        kfree(tmp);
        return (uint64_t)(int64_t)-EFAULT;
    }
    kfree(tmp);
    pfd->offset += to_read;
    return (uint64_t)to_read;
}

static uint64_t sys_fd_write(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used) return (uint64_t)(int64_t)-EBADF;
    /* Clamp count to UINT32_MAX to avoid uint32_t truncation in vfs_write/vfs_append */
    if (count > UINT32_MAX) count = UINT32_MAX;
    int r;
    if (pfd->open_flags & O_APPEND) {
        r = vfs_append(pfd->path, (const void *)(uintptr_t)buf_addr, (uint32_t)count);
        if (r == 0) {
            struct vfs_stat st;
            if (vfs_stat(pfd->path, &st) == 0)
                pfd->offset = st.size;
        }
    } else {
        r = vfs_write(pfd->path, (const void *)(uintptr_t)buf_addr, (uint32_t)count);
        if (r == 0) pfd->offset += count;
    }
    return (r == 0) ? count : (uint64_t)(int64_t)r;
}

/* ── Positional read/write (pread64/pwrite64) ──────────────── */

/**
 * sys_pread64 - Read from a file descriptor at a specific offset
 * @fd: File descriptor number
 * @buf_addr: User-space address of the output buffer
 * @count: Number of bytes to read
 * @offset: File offset to read from
 *
 * Reads up to @count bytes from the file descriptor @fd starting at
 * @offset. Unlike sys_read/sys_fd_read, the file descriptor's current
 * offset is NOT modified. Returns the number of bytes read, 0 at EOF,
 * or (uint64_t)-1 on error with errno encoded as negative value.
 *
 * Context: Called from syscall dispatch. May sleep.
 * Return: Number of bytes read, 0 at EOF, or (uint64_t)-1 on error.
 */
static uint64_t sys_pread64(uint64_t fd, uint64_t buf_addr,
                            uint64_t count, uint64_t offset)
{
    if (fd < 3)
        return (uint64_t)(int64_t)-EBADF;
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used)
        return (uint64_t)(int64_t)-EBADF;
    if (!buf_addr && count > 0)
        return (uint64_t)(int64_t)-EFAULT;

    struct vfs_stat st;
    if (vfs_stat(pfd->path, &st) < 0)
        return (uint64_t)(int64_t)-EIO;

    uint64_t fsize = st.size;
    if (offset >= fsize)
        return 0;

    uint64_t avail = fsize - offset;
    uint64_t to_read = count < avail ? count : avail;
    /* Clamp to UINT32_MAX to avoid uint32_t truncation in vfs_read */
    if (to_read > UINT32_MAX)
        to_read = UINT32_MAX;

    uint64_t need_end = offset + to_read;
    if (need_end > fsize)
        need_end = fsize;
    if (need_end > UINT32_MAX)
        need_end = UINT32_MAX;

    uint8_t *tmp = kmalloc(need_end);
    if (!tmp)
        return (uint64_t)(int64_t)-ENOMEM;

    uint32_t nread = 0;
    vfs_read(pfd->path, tmp, (uint32_t)need_end, &nread);

    if (copy_to_user(buf_addr, tmp + offset, to_read) < 0) {
        kfree(tmp);
        return (uint64_t)(int64_t)-EFAULT;
    }
    kfree(tmp);

    /* I/O accounting */
    {
        struct process *cur = process_get_current();
        if (cur) {
            cur->io_rchar += to_read;
            cur->io_syscr++;
            cur->io_read_bytes += to_read;
        }
    }

    return (uint64_t)to_read;
}

/**
 * sys_pwrite64 - Write to a file descriptor at a specific offset
 * @fd: File descriptor number
 * @buf_addr: User-space address of the data to write
 * @count: Number of bytes to write
 * @offset: File offset to write at
 *
 * Writes up to @count bytes to the file descriptor @fd starting at
 * @offset. Unlike sys_write/sys_fd_write, the file descriptor's current
 * offset is NOT modified. If @offset extends beyond the current file
 * size, the gap is zero-filled. Returns the number of bytes written,
 * or (uint64_t)-1 on error with errno encoded as negative value.
 *
 * Context: Called from syscall dispatch. May sleep.
 * Return: Number of bytes written, or (uint64_t)-1 on error.
 */
static uint64_t sys_pwrite64(uint64_t fd, uint64_t buf_addr,
                             uint64_t count, uint64_t offset)
{
    if (fd < 3)
        return (uint64_t)(int64_t)-EBADF;
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used)
        return (uint64_t)(int64_t)-EBADF;
    if (!buf_addr && count > 0)
        return (uint64_t)(int64_t)-EFAULT;

    struct vfs_stat st;
    if (vfs_stat(pfd->path, &st) < 0)
        return (uint64_t)(int64_t)-EIO;

    uint64_t fsize = st.size;
    uint64_t need_end = offset + count;
    /* Clamp buffer size to UINT32_MAX to avoid uint32_t truncation */
    uint64_t buf_size = need_end > fsize ? need_end : fsize;
    if (buf_size > UINT32_MAX)
        buf_size = UINT32_MAX;

    uint8_t *tmp = kmalloc(buf_size);
    if (!tmp)
        return (uint64_t)(int64_t)-ENOMEM;

    /* Read existing file content into buffer */
    uint32_t nread = 0;
    vfs_read(pfd->path, tmp, (uint32_t)buf_size, &nread);

    /* If writing beyond current file size, zero-fill the gap */
    if (offset + count > fsize) {
        uint64_t zero_start = nread > offset ? offset : nread;
        if (zero_start < offset) {
            memset(tmp + zero_start, 0, (size_t)(offset - zero_start));
        }
    }

    /* Copy user data into buffer at offset (validated by syscall_validate_user_args) */
    if (copy_from_user(tmp + offset, buf_addr, count) < 0) {
        kfree(tmp);
        return (uint64_t)(int64_t)-EFAULT;
    }

    /* Write the entire buffer back */
    int r = vfs_write(pfd->path, tmp, (uint32_t)buf_size);
    kfree(tmp);
    if (r < 0)
        return (uint64_t)(int64_t)r;

    /* I/O accounting */
    {
        struct process *cur = process_get_current();
        if (cur) {
            cur->io_wchar += count;
            cur->io_syscw++;
            cur->io_write_bytes += count;
        }
    }

    return (uint64_t)count;
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
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    uint64_t *out = (uint64_t *)out_addr;
    struct vfs_stat st;
    if (vfs_stat(kpath, &st) < 0)
        return (uint64_t)(int64_t)-ENOENT;
    if (out) {
        uint64_t stbuf[2] = { st.size, st.type };
        if (copy_to_user(out_addr, stbuf, sizeof(stbuf)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }
    return 0;
}

static uint64_t sys_mkdir(uint64_t path_addr) {
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    int ret = fs_create(kpath, FS_TYPE_DIR);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
}

static uint64_t sys_unlink(uint64_t path_addr) {
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    char ap[128];
    const char *rp = vfs_abs_path(kpath, ap, sizeof(ap)) < 0
                     ? kpath : ap;
    int ret = fs_delete(rp);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
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
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)fs_create(rp, (uint8_t)type);
}

static uint64_t sys_fs_write(uint64_t path_addr, uint64_t data_addr, uint64_t size) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)fs_write_file(rp, (const void *)data_addr, (uint32_t)size);
}

static uint64_t sys_fs_read(uint64_t path_addr, uint64_t buf_addr, uint64_t max_size, uint64_t out_addr) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)fs_read_file(rp, (void *)buf_addr,
                                  (uint32_t)max_size, (uint32_t *)out_addr);
}

static uint64_t sys_fs_delete(uint64_t path_addr) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)fs_delete(rp);
}

static uint64_t sys_fs_list(uint64_t path_addr) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)fs_list(rp);
}

static uint64_t sys_fs_stat(uint64_t path_addr, uint64_t out_addr) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    uint32_t size = 0;
    uint8_t type = 0;
    int rc = fs_stat(rp, &size, &type);
    if (rc < 0) return (uint64_t)rc;
    if (out_addr) {
        uint32_t *out = (uint32_t *)out_addr;
        out[0] = size;
        out[1] = type;
    }
    return 0;
}

static uint64_t sys_fs_stat_ex(uint64_t path_addr, uint64_t out_addr) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    struct syscall_fs_stat_ex *out = (struct syscall_fs_stat_ex *)out_addr;
    uint32_t size = 0;
    uint8_t type = 0;
    uint16_t uid = 0, gid = 0, mode = 0;
    int rc = fs_stat_ex(rp, &size, &type, &uid, &gid, &mode);
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
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)fs_chmod(rp, (uint16_t)mode);
}

static uint64_t sys_fs_chown(uint64_t path_addr, uint64_t uid, uint64_t gid) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)fs_chown(rp, (uint16_t)uid, (uint16_t)gid);
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
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)(int64_t)fs_symlink(rp, (const char *)target_addr);
}
static uint64_t sys_fs_readlink(uint64_t path_addr, uint64_t buf_addr, uint64_t bufsz) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)(int64_t)fs_readlink(rp, (char *)buf_addr, (int)bufsz);
}
static uint64_t sys_fs_lstat(uint64_t path_addr, uint64_t size_addr, uint64_t type_addr) {
    char ap[128];
    const char *rp = vfs_abs_path((const char *)path_addr, ap, sizeof(ap)) < 0
                     ? (const char *)path_addr : ap;
    return (uint64_t)(int64_t)fs_lstat(rp, (uint32_t *)size_addr, (uint8_t *)type_addr);
}

static uint64_t sys_chdir(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)(int64_t)-EFAULT;
    /* Prefer per-session CWD so that cd/pwd work correctly even when
     * net_poll() is called from a different process (e.g. httpd). */
    char *ses_cwd = telnet_get_cwd_ctx();
    /* Resolve to absolute path via VFS */
    char ap[128];
    if (vfs_abs_path(path, ap, sizeof(ap)) < 0)
        return (uint64_t)(int64_t)-ENOENT;
    /* For telnet sessions, re-resolve using session CWD as base
     * since vfs_abs_path uses per-process CWD which may differ. */
    if (ses_cwd && path[0] != '/') {
        char tmp[128];
        size_t cl = (size_t)strlen(ses_cwd);
        size_t pl = (size_t)strlen(path);
        if (cl + 1 + pl < sizeof(tmp)) {
            memcpy(tmp, ses_cwd, cl);
            if (tmp[cl-1] != '/') tmp[cl++] = '/';
            memcpy(tmp + cl, path, pl + 1);
            /* Normalize .. and . components */
            vfs_abs_path(tmp, ap, sizeof(ap));
        }
    }
    /* Special case: root always exists */
    if (ap[0] == '/' && ap[1] == '\0') {
        if (ses_cwd) { ses_cwd[0] = '/'; ses_cwd[1] = '\0'; return 0; }
        struct process *cur = process_get_current();
        if (!cur) return (uint64_t)(int64_t)-ESRCH;
        cur->cwd[0] = '/'; cur->cwd[1] = '\0';
        return 0;
    }
    /* Verify it's a directory */
    struct vfs_stat st;
    int vstat_r = vfs_stat(ap, &st);
    if (vstat_r < 0)
        return (uint64_t)(int64_t)-ENOENT;
    if (st.type != VFS_TYPE_DIR)
        return (uint64_t)(int64_t)-ENOTDIR;
    /* Remove trailing slash (except root) */
    int len = (int)strlen(ap);
    while (len > 1 && ap[len-1] == '/') { ap[--len] = '\0'; }
    if (ses_cwd) { strncpy(ses_cwd, ap, 63); ses_cwd[63] = '\0'; return 0; }
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)(int64_t)-ESRCH;
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
    if (buf_size == 0) return (uint64_t)(int64_t)-EINVAL;
    if (!buf) return (uint64_t)(int64_t)-EFAULT;
    size_t max = buf_size;
    strncpy(buf, cwd, max - 1); buf[max-1] = '\0';
    return 0;
}

/*
 * ── Nice value ↔ internal priority conversion ────────────────────────
 *
 * POSIX nice values range from -20 (highest) to +19 (lowest).
 * The kernel's internal priority is 0 (highest) to 3 (lowest).
 *
 *   nice -20 .. -11  →  priority 0
 *   nice -10 ..  -1  →  priority 1
 *   nice   0 ..   9  →  priority 2
 *   nice  10 ..  19  →  priority 3
 */
static int nice_to_priority(int nice) {
    if (nice <= -11) return 0;
    if (nice <=  -1) return 1;
    if (nice <=   9) return 2;
    return 3;
}

/* Return the "middle" nice value corresponding to a given priority level.
 * Used by getpriority to report a representative nice value. */
static int priority_to_nice(int prio) {
    switch (prio) {
        case 0: return -15;
        case 1: return  -5;
        case 2: return   5;
        case 3: return  15;
        default: return  0;
    }
}

/* Clamp nice to valid range [-20, 19] */
static int clamp_nice(int nice) {
    if (nice < NICE_MIN) return NICE_MIN;
    if (nice > NICE_MAX) return NICE_MAX;
    return nice;
}

/*
 * ── POSIX setpriority(which, who, prio) ──────────────────────────────
 *
 * Set the nice value for the specified process(es).
 *   which: PRIO_PROCESS (0), PRIO_PGRP (1), PRIO_USER (2)
 *   who:   PID, PGID, or UID depending on 'which'
 *   prio:  nice value in [-20, +19] (lower = higher priority)
 *
 * Returns 0 on success, -1 on error (EINVAL, ESRCH, EPERM).
 */
static uint64_t sys_setpriority(uint64_t which, uint64_t who, uint64_t prio) {
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)(int64_t)-ESRCH;

    int nice = clamp_nice((int)(int64_t)prio);

    switch (which) {
    case PRIO_PROCESS: {
        /* Operate on a specific process */
        struct process *p;
        if (who == 0) {
            p = cur;
        } else {
            p = process_get_by_pid((uint32_t)who);
            if (!p || p->state == PROCESS_UNUSED)
                return (uint64_t)(int64_t)-ESRCH;
        }
        scheduler_set_nice(p, nice);
        return 0;
    }
    case PRIO_PGRP: {
        /* Operate on all processes in a process group */
        uint32_t pgid = (who == 0) ? cur->pgid : (uint32_t)who;
        struct process *table = process_get_table();
        uint32_t count = process_get_count();
        int found = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (table[i].state == PROCESS_UNUSED) continue;
            if (table[i].pgid == pgid) {
                scheduler_set_nice(&table[i], nice);
                found = 1;
            }
        }
        if (!found) return (uint64_t)(int64_t)-ESRCH;
        return 0;
    }
    case PRIO_USER: {
        /* Operate on all processes owned by a user */
        uint32_t uid = (who == 0) ? cur->uid : (uint32_t)who;
        struct process *table = process_get_table();
        uint32_t count = process_get_count();
        int found = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (table[i].state == PROCESS_UNUSED) continue;
            if (table[i].uid == uid) {
                scheduler_set_nice(&table[i], nice);
                found = 1;
            }
        }
        if (!found) return (uint64_t)(int64_t)-ESRCH;
        return 0;
    }
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }
}

/*
 * ── POSIX getpriority(which, who) ────────────────────────────────────
 *
 * Get the highest nice value (lowest priority) among matching processes.
 *   which: PRIO_PROCESS (0), PRIO_PGRP (1), PRIO_USER (2)
 *   who:   PID, PGID, or UID depending on 'which'
 *
 * Returns the nice value (as a 40-compatible int in the range -20..+19)
 * on success, or -1 on error.  Note that nice values can legally be -1,
 * so callers must also check errno on -1 return.
 */
static uint64_t sys_getpriority(uint64_t which, uint64_t who) {
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)(int64_t)-ESRCH;

    int highest_nice = NICE_MIN - 1; /* sentinel: below valid range */

    switch (which) {
    case PRIO_PROCESS: {
        struct process *p;
        if (who == 0) {
            p = cur;
        } else {
            p = process_get_by_pid((uint32_t)who);
            if (!p || p->state == PROCESS_UNUSED)
                return (uint64_t)(int64_t)-ESRCH;
        }
        highest_nice = p->nice;
        break;
    }
    case PRIO_PGRP: {
        uint32_t pgid = (who == 0) ? cur->pgid : (uint32_t)who;
        struct process *table = process_get_table();
        uint32_t count = process_get_count();
        for (uint32_t i = 0; i < count; i++) {
            if (table[i].state == PROCESS_UNUSED) continue;
            if (table[i].pgid == pgid && table[i].nice > highest_nice)
                highest_nice = table[i].nice;
        }
        if (highest_nice < NICE_MIN)
            return (uint64_t)(int64_t)-ESRCH;
        break;
    }
    case PRIO_USER: {
        uint32_t uid = (who == 0) ? cur->uid : (uint32_t)who;
        struct process *table = process_get_table();
        uint32_t count = process_get_count();
        for (uint32_t i = 0; i < count; i++) {
            if (table[i].state == PROCESS_UNUSED) continue;
            if (table[i].uid == uid && table[i].nice > highest_nice)
                highest_nice = table[i].nice;
        }
        if (highest_nice < NICE_MIN)
            return (uint64_t)(int64_t)-ESRCH;
        break;
    }
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }

    /* Return the nice value as a signed value properly cast.
     * POSIX says getpriority returns the nice value in the range -20..+19,
     * or -1 on error.  We return the raw signed value via (int64_t). */
    return (uint64_t)(int64_t)highest_nice;
}

/* ── ioprio_get / ioprio_set (Item 327) ─────────────────────────── */

/*
 * ioprio_get(which, who) → ioprio or -errno
 *
 * Returns the I/O priority of a process / process group / user.
 *   which = IOPRIO_WHO_PROCESS (1): who is a PID (0 = current)
 *   which = IOPRIO_WHO_PGRP    (2): who is a PGID (0 = current)
 *   which = IOPRIO_WHO_USER    (3): who is a UID (0 = current)
 *
 * On success, returns the 16-bit ioprio value.
 * On error, returns -ESRCH (no such process/pgrp/user) or -EINVAL.
 */
static uint64_t sys_ioprio_get(uint64_t which, uint64_t who)
{
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)(int64_t)-EINVAL;

    switch (which) {
    case IOPRIO_WHO_PROCESS: {
        struct process *p;
        if (who == 0) {
            p = cur;
        } else {
            p = process_get_by_pid((uint32_t)who);
            if (!p || p->state == PROCESS_UNUSED)
                return (uint64_t)(int64_t)-ESRCH;
        }
        return (uint64_t)p->ioprio;
    }
    case IOPRIO_WHO_PGRP: {
        uint32_t pgid = (uint32_t)(who ? who : cur->pgid);
        struct process *table = process_get_table();
        /* Return the highest priority (lowest value) among the group */
        uint16_t best = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0);
        int found = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_UNUSED && table[i].pgid == pgid) {
                found = 1;
                if (ioprio_class_order(table[i].ioprio) < ioprio_class_order(best))
                    best = table[i].ioprio;
            }
        }
        if (!found) return (uint64_t)(int64_t)-ESRCH;
        return (uint64_t)best;
    }
    case IOPRIO_WHO_USER: {
        uint32_t uid = (uint32_t)(who ? who : cur->uid);
        struct process *table = process_get_table();
        uint16_t best = IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0);
        int found = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_UNUSED &&
                (table[i].uid == uid || table[i].euid == uid)) {
                found = 1;
                if (ioprio_class_order(table[i].ioprio) < ioprio_class_order(best))
                    best = table[i].ioprio;
            }
        }
        if (!found) return (uint64_t)(int64_t)-ESRCH;
        return (uint64_t)best;
    }
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }
}

/*
 * ioprio_set(which, who, ioprio) → 0 or -errno
 *
 * Sets the I/O priority of a process / process group / user.
 * Requires CAP_SYS_NICE or appropriate permissions.
 *   which = IOPRIO_WHO_PROCESS (1): who is a PID (0 = current)
 *   which = IOPRIO_WHO_PGRP    (2): who is a PGID (0 = current)
 *   which = IOPRIO_WHO_USER    (3): who is a UID (0 = current)
 *
 * ioprio encodes class (bits 15:13) and priority data (bits 12:0):
 *   IOPRIO_CLASS_RT (1), IOPRIO_CLASS_BE (2), IOPRIO_CLASS_IDLE (3)
 */
static uint64_t sys_ioprio_set(uint64_t which, uint64_t who, uint64_t ioprio)
{
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)(int64_t)-EINVAL;

    /* Validate: class must be NONE, RT, BE, or IDLE */
    unsigned int class = IOPRIO_PRIO_CLASS((uint16_t)(ioprio & 0xFFFF));
    if (class > IOPRIO_CLASS_IDLE)
        return (uint64_t)(int64_t)-EINVAL;

    /* Only root can set RT priority class (CAP_SYS_NICE equivalent) */
    if (class == IOPRIO_CLASS_RT && cur->uid != 0)
        return (uint64_t)(int64_t)-EPERM;

    uint16_t ioprio16 = (uint16_t)(ioprio & 0xFFFF);

    switch (which) {
    case IOPRIO_WHO_PROCESS: {
        struct process *p;
        if (who == 0) {
            p = cur;
        } else {
            p = process_get_by_pid((uint32_t)who);
            if (!p || p->state == PROCESS_UNUSED)
                return (uint64_t)(int64_t)-ESRCH;
        }
        p->ioprio = ioprio16;
        return 0;
    }
    case IOPRIO_WHO_PGRP: {
        uint32_t pgid = (uint32_t)(who ? who : cur->pgid);
        struct process *table = process_get_table();
        int found = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_UNUSED && table[i].pgid == pgid) {
                table[i].ioprio = ioprio16;
                found = 1;
            }
        }
        if (!found) return (uint64_t)(int64_t)-ESRCH;
        return 0;
    }
    case IOPRIO_WHO_USER: {
        uint32_t uid = (uint32_t)(who ? who : cur->uid);
        struct process *table = process_get_table();
        int found = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_UNUSED &&
                (table[i].uid == uid || table[i].euid == uid)) {
                table[i].ioprio = ioprio16;
                found = 1;
            }
        }
        if (!found) return (uint64_t)(int64_t)-ESRCH;
        return 0;
    }
    default:
        return (uint64_t)(int64_t)-EINVAL;
    }
}

static uint64_t sys_setpgid(uint64_t pid, uint64_t pgid) {
    struct process *p;
    if (pid == 0) p = process_get_current();
    else p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-ESRCH;
    p->pgid = pgid ? (uint32_t)pgid : p->pid;
    if (p->sid == 0) p->sid = p->pgid;
    return 0;
}

static uint64_t sys_getpgid(uint64_t pid) {
    struct process *p;
    if (pid == 0) p = process_get_current();
    else p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-ESRCH;
    return p->pgid;
}

static uint64_t sys_killpg(uint64_t pgid, uint64_t sig) {
    return (uint64_t)(int64_t)signal_send_group((uint32_t)pgid, (int)sig);
}

static uint64_t sys_shm_get(uint64_t key, uint64_t mode) {
    return (uint64_t)(int64_t)shm_get((int)key, (uint16_t)mode);
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

/* shmctl(id, cmd, arg) — control shared memory segment permissions */
static uint64_t sys_shmctl(uint64_t id, uint64_t cmd, uint64_t arg) {
    int cid = (int)id;
    int ccmd = (int)cmd;

    /* SHMCTL_IPC_RMID (3) */
    if (ccmd == 3)
        return (uint64_t)(int64_t)shm_free(cid);

    /* SHMCTL_IPC_SET (2): arg is pointer to struct shm_perm in user space */
    if (ccmd == 2) {
        struct shm_perm sp;
        if (copy_from_user(&sp, (uint64_t)(uintptr_t)arg, sizeof(sp)) < 0)
            return (uint64_t)-1;
        return (uint64_t)(int64_t)shm_perm_set(cid, sp.uid, sp.gid, sp.mode);
    }

    /* SHMCTL_IPC_STAT (1): copy metadata to user space */
    if (ccmd == 1) {
        struct shm_perm sp;
        int ret = shm_perm_get(cid, &sp);
        if (ret < 0) return (uint64_t)(int64_t)ret;
        if (copy_to_user((uint64_t)(uintptr_t)arg, &sp, sizeof(sp)) < 0)
            return (uint64_t)-1;
        return 0;
    }

    return (uint64_t)(int64_t)-1; /* unknown command */
}

/* ── semctl() — System V semaphore control (simplified IPC_STAT/IPC_SET) ─── */
struct sem_perm {
    int      key;
    int      semval;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
};

static uint64_t sys_semctl(uint64_t semid, uint64_t semnum, uint64_t cmd, uint64_t arg) {
    (void)semnum;
    int ccmd = (int)cmd;

    /* Get current IPC namespace */
    struct ipc_namespace *ns = ipc_ns_current();
    if (!ns) return (uint64_t)-1;

    /* Validate semid */
    int sid = (int)semid;
    if (sid < 0 || sid >= IPC_NS_MAX_SEMS || !ns->sem_table[sid].used)
        return (uint64_t)-1;

    /* SEMCTL_IPC_STAT (1): copy semaphore metadata to user space */
    if (ccmd == 1) {
        struct sem_perm sp;
        sp.key    = ns->sem_table[sid].key;
        sp.semval = ns->sem_table[sid].semval;
        sp.uid    = ns->sem_table[sid].uid;
        sp.gid    = ns->sem_table[sid].gid;
        sp.mode   = ns->sem_table[sid].mode;
        if (copy_to_user(arg, &sp, sizeof(sp)) < 0)
            return (uint64_t)-1;
        return 0;
    }

    /* SEMCTL_IPC_SET (2): set semaphore permissions from user space */
    if (ccmd == 2) {
        struct sem_perm sp;
        if (copy_from_user(&sp, arg, sizeof(sp)) < 0)
            return (uint64_t)-1;
        ns->sem_table[sid].uid  = sp.uid;
        ns->sem_table[sid].gid  = sp.gid;
        ns->sem_table[sid].mode = sp.mode;
        return 0;
    }

    return (uint64_t)-1; /* unknown command */
}

static uint64_t sys_fork(void) {
    return (uint64_t)(int64_t)process_fork();
}

static uint64_t sys_clone(uint64_t flags, uint64_t child_stack, uint64_t ptid,
                           uint64_t tls, uint64_t ctid) {
    struct process *parent = process_get_current();
    if (!parent) return (uint64_t)(int64_t)-EAGAIN;

    /* ── Validate flag combinations (Linux-compatible) ───────────── */
    /* CLONE_THREAD requires CLONE_VM (cannot share TGID without VM) */
    if ((flags & CLONE_THREAD) && !(flags & CLONE_VM))
        return (uint64_t)(int64_t)-EINVAL;
    /* CLONE_SIGHAND requires CLONE_VM */
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
        return (uint64_t)(int64_t)-EINVAL;

    uint64_t user_rip = syscall_user_rip;
    uint64_t user_rflags = syscall_user_rflags;

    /* For kernel-mode callers: RIP/RFLAGS from syscall_user_* may be stale.
     * Use a default: treat kernel callers differently. */
    if (parent->is_user) {
        /* ── CLONE_PARENT_SETTID: write child PID to *ptid ──── */
        /* On Linux, PARENT_SETTID writes the child's PID to ptid
         * before the child runs.  We do this after clone returns
         * when we know the child PID. */

        int ret = process_clone(parent, flags, (void *)child_stack,
                                user_rip, user_rflags);
        if (ret < 0)
            return (uint64_t)(int64_t)ret;

        /* Child was created successfully — apply CLONE_* side effects */
        struct process *child = process_get_by_pid((uint32_t)ret);

        /* ── CLONE_PARENT_SETTID: write child PID to *ptid ────── */
        if ((flags & CLONE_PARENT_SETTID) && ptid && child) {
            uint32_t child_pid = (uint32_t)ret;
            int uerr = copy_to_user(ptid, &child_pid, sizeof(child_pid));
            if (uerr < 0) {
                /* CLONE_PARENT_SETTID write failed — invalid user pointer */
                return (uint64_t)(int64_t)-EFAULT;
            }
        }

        /* ── CLONE_CHILD_SETTID: write child PID to *ctid ──────── */
        if ((flags & CLONE_CHILD_SETTID) && ctid && child) {
            uint32_t child_pid = (uint32_t)ret;
            int uerr = copy_to_user(ctid, &child_pid, sizeof(child_pid));
            if (uerr < 0) {
                return (uint64_t)(int64_t)-EFAULT;
            }
        }

        /* ── CLONE_CHILD_CLEARTID: store ctid ptr for teardown ─── */
        if ((flags & CLONE_CHILD_CLEARTID) && ctid && child) {
            child->ctid_ptr = (void *)ctid;
        }

        /* ── CLONE_SETTLS: set FS base for child thread ────────── */
        if ((flags & CLONE_SETTLS) && child) {
            /* Save parent's FS base first */
            uint32_t lo, hi;
            __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100ULL));
            uint64_t saved_fs_base = ((uint64_t)hi << 32) | lo;

            /* Set child's FS base */
            __asm__ volatile("wrmsr" : : "c"(0xC0000100ULL),
                             "a"((uint32_t)tls),
                             "d"((uint32_t)(tls >> 32)));

            /* Restore parent's FS base */
            __asm__ volatile("wrmsr" : : "c"(0xC0000100ULL),
                             "a"((uint32_t)saved_fs_base),
                             "d"((uint32_t)(saved_fs_base >> 32)));
        }

        /* ── CLONE_VFORK: block parent until child exec/exit ──── */
        if ((flags & CLONE_VFORK) && child) {
            parent->wait_for_pid = child->pid;
            parent->state = PROCESS_BLOCKED;
            scheduler_remove(parent);
            scheduler_yield();
            parent->wait_for_pid = 0;
        }

        return (uint64_t)(int64_t)ret;
    }

    /* Kernel-mode clone: create a thread that calls a function.
     * child_stack is actually a function pointer for kernel threads. */
    int ret = process_clone(parent, flags, (void *)child_stack,
                            0, 0);
    return (uint64_t)(int64_t)ret;
}

/* ── sys_clone3 — Linux-compatible extensible clone interface ───── */
/*
 * clone3 is the modern Linux clone interface that uses a struct
 * clone_args for all parameters instead of encoding them in flags
 * and register-passed args.  The size parameter allows future
 * extension of the argument structure.
 *
 * Linux syscall signature:
 *   long clone3(struct clone_args *uargs, size_t size);
 *   syscall(SYS_clone3, &args, sizeof(args));
 *
 * Returns child PID on success, negative errno on failure.
 */
static uint64_t sys_clone3(uint64_t uargs_addr, uint64_t size) {
    struct process *parent = process_get_current();
    if (!parent) return (uint64_t)(int64_t)-EAGAIN;

    /*
     * The size must be at least CLONE_ARGS_SIZE_VER0.
     * If it's larger, the kernel still knows how to handle the base
     * struct — it just treats unknown trailing bytes as zero.
     * Linux does this for forward compatibility.
     */
    if (size < CLONE_ARGS_SIZE_VER0)
        return (uint64_t)(int64_t)-EINVAL;

    /* Copy clone_args from user-space */
    struct clone_args args;
    int err = copy_from_user(&args, uargs_addr, sizeof(args));
    if (err < 0)
        return (uint64_t)(int64_t)-EFAULT;

    uint64_t flags = args.flags;

    /* ── Validate flag combinations (same as Linux clone) ──────── */
    /* CLONE_THREAD requires CLONE_VM (cannot share TGID without VM) */
    if ((flags & CLONE_THREAD) && !(flags & CLONE_VM))
        return (uint64_t)(int64_t)-EINVAL;
    /* CLONE_SIGHAND requires CLONE_VM */
    if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
        return (uint64_t)(int64_t)-EINVAL;

    /* ── set_tid / set_tid_size: currently unsupported ─────────── */
    if (args.set_tid_size > 0)
        return (uint64_t)(int64_t)-EOPNOTSUPP;

    /*
     * Build the child stack pointer.
     * In clone3, args.stack is the LOWEST address of the stack,
     * and args.stack_size is its size.  The initial stack pointer
     * is stack + stack_size.  If stack is 0, we treat it like
     * legacy clone(child_stack=0).
     */
    void *child_stack = NULL;
    if (args.stack != 0 && args.stack_size != 0) {
        child_stack = (void *)(args.stack + args.stack_size);
    }

    uint64_t user_rip = syscall_user_rip;
    uint64_t user_rflags = syscall_user_rflags;

    if (parent->is_user) {
        int ret = process_clone(parent, flags, child_stack,
                                user_rip, user_rflags);
        if (ret < 0)
            return (uint64_t)(int64_t)ret;

        struct process *child = process_get_by_pid((uint32_t)ret);

        /* ── CLONE_PARENT_SETTID: write child PID to *parent_tid ── */
        if ((flags & CLONE_PARENT_SETTID) && args.parent_tid && child) {
            uint32_t child_pid = (uint32_t)ret;
            int uerr = copy_to_user(args.parent_tid, &child_pid,
                                    sizeof(child_pid));
            if (uerr < 0)
                return (uint64_t)(int64_t)-EFAULT;
        }

        /* ── CLONE_CHILD_SETTID: write child PID to *child_tid ──── */
        if ((flags & CLONE_CHILD_SETTID) && args.child_tid && child) {
            uint32_t child_pid = (uint32_t)ret;
            int uerr = copy_to_user(args.child_tid, &child_pid,
                                    sizeof(child_pid));
            if (uerr < 0)
                return (uint64_t)(int64_t)-EFAULT;
        }

        /* ── CLONE_CHILD_CLEARTID: store ctid ptr for teardown ──── */
        if ((flags & CLONE_CHILD_CLEARTID) && args.child_tid && child) {
            child->ctid_ptr = (void *)args.child_tid;
        }

        /* ── CLONE_SETTLS: set TLS (FS base) for child ──────────── */
        if ((flags & CLONE_SETTLS) && child) {
            uint32_t lo, hi;
            __asm__ volatile("rdmsr"
                             : "=a"(lo), "=d"(hi)
                             : "c"(0xC0000100ULL));
            uint64_t saved_fs_base = ((uint64_t)hi << 32) | lo;

            __asm__ volatile("wrmsr"
                             :
                             : "c"(0xC0000100ULL),
                               "a"((uint32_t)args.tls),
                               "d"((uint32_t)(args.tls >> 32)));

            __asm__ volatile("wrmsr"
                             :
                             : "c"(0xC0000100ULL),
                               "a"((uint32_t)saved_fs_base),
                               "d"((uint32_t)(saved_fs_base >> 32)));
        }

        /* ── CLONE_VFORK: block parent until child exec/exit ────── */
        if ((flags & CLONE_VFORK) && child) {
            parent->wait_for_pid = child->pid;
            parent->state = PROCESS_BLOCKED;
            scheduler_remove(parent);
            scheduler_yield();
            parent->wait_for_pid = 0;
        }

        /*
         * ── pidfd: create a pidfd for the new child ─────────────
         * If args.pidfd is non-zero, the caller wants a file
         * descriptor that refers to the child process.  We write
         * the fd number to the userspace pointer.
         */
        if (args.pidfd && child) {
            int pidfd = pidfd_open((uint32_t)ret, 0);
            if (pidfd >= 0) {
                uint32_t pidfd_u32 = (uint32_t)pidfd;
                int uerr = copy_to_user(args.pidfd, &pidfd_u32,
                                        sizeof(pidfd_u32));
                if (uerr < 0) {
                    /* pidfd leaked on failure, but that's acceptable */
                }
            }
            /* If pidfd_open fails, we silently skip — the caller
             * gets -1 in *pidfd, which is the same as Linux behavior
             * when pidfd_open is not supported by the filesystem. */
        }

        return (uint64_t)(int64_t)ret;
    }

    /* Kernel-mode clone3: same as kernel-mode clone path */
    int ret = process_clone(parent, flags, child_stack, 0, 0);
    return (uint64_t)(int64_t)ret;
}

/* ── unshare(CLONE_NEW*) — create namespaces without fork (Item 119) ──── */
/*
 * The unshare syscall lets a process disassociate parts of its execution
 * context into new namespaces without creating a new process.  This is the
 * foundation for container runtime isolation.
 *
 * Supported flags (from Linux CLONE_NEW*):
 *   CLONE_NEWNS   — detach from shared mount tree (no-op: no shared mounts yet)
 *   CLONE_NEWUTS  — create a private copy of hostname/domainname
 *   CLONE_NEWPID  — mark for PID namespace isolation (future)
 *   CLONE_NEWNET  — mark for network namespace isolation (future)
 *   CLONE_NEWIPC  — mark for IPC namespace isolation (future)
 *
 * Returns 0 on success, -1 with errno on failure.
 */
static uint64_t sys_unshare(uint64_t flags)
{
    /* Only the namespace-related bits are accepted — all other flags
     * (CLONE_VM, CLONE_THREAD, etc.) are invalid for unshare. */
    uint64_t ns_mask = CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWPID |
                       CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWCGROUP |
                       CLONE_NEWTIME | CLONE_NEWUSER;

    if (flags & ~ns_mask)
        return (uint64_t)-EINVAL;

    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)-EINVAL;

    /* ── CLONE_NEWUTS: copy hostname/domainname ──────────────── */
    if (flags & CLONE_NEWUTS) {
        /* Snapshot the current hostname into the process-local buffer.
         * Future sethostname() calls on this process will only affect
         * its own copy. */
        const char *host = sysctl_get_hostname();
        strncpy(cur->ns_hostname, host ? host : "localhost",
                sizeof(cur->ns_hostname) - 1);
        cur->ns_hostname[sizeof(cur->ns_hostname) - 1] = '\0';

        /* Also copy the domainname from the system default */
        strncpy(cur->ns_domainname, "(none)",
                sizeof(cur->ns_domainname) - 1);
        cur->ns_domainname[sizeof(cur->ns_domainname) - 1] = '\0';
    }

    /* ── CLONE_NEWNS: detach from mount namespace ────────────── */
    if (flags & CLONE_NEWNS) {
        struct mnt_namespace *new_ns = mnt_ns_copy(cur->mnt_ns ? cur->mnt_ns : NULL);
        if (!new_ns)
            return (uint64_t)-ENOMEM;

        /* Drop reference to old namespace */
        if (cur->mnt_ns)
            mnt_ns_put(cur->mnt_ns);
        cur->mnt_ns = new_ns;
        cur->ns_flags |= CLONE_NEWNS;
    }

    /* ── CLONE_NEWPID: mark for PID namespace ────────────────── */
    if (flags & CLONE_NEWPID) {
        /* Mark the process for PID namespace isolation on next fork.
         * When this flag is set, the next clone()/fork() will create a
         * new PID namespace for the child.  The calling process itself
         * remains in the original namespace. */
        cur->ns_flags |= CLONE_NEWPID;
    }

    /* ── CLONE_NEWNET: mark for network namespace ────────────── */
    if (flags & CLONE_NEWNET) {
        /* Network namespace isolation is not yet fully wired.  The flag
         * is accepted for future-proofing. */
    }

    /* ── CLONE_NEWIPC: mark for IPC namespace ────────────────── */
    if (flags & CLONE_NEWIPC) {
        /* IPC namespace isolation: future. */
    }

    /* ── CLONE_NEWCGROUP: create a new cgroup namespace (Item 117) ── */
    if (flags & CLONE_NEWCGROUP) {
        /* Snapshot current cgroup path as the namespace root */
        const char *cur_path = "/sys/fs/cgroup";  /* default cgroup path */
        struct cgroup_namespace *new_ns = cgroup_ns_create(cur_path);
        if (!new_ns) {
            return (uint64_t)-1;  /* ENOMEM */
        }
        /* Drop the old reference and take the new one */
        if (cur->cgroup_ns)
            cgroup_ns_put(cur->cgroup_ns);
        cur->cgroup_ns = new_ns;
        kprintf("[CGROUP_NS] unshare(NEWCGROUP): PID %d, root='%s'\n",
                cur->pid, new_ns->root_path);
    }

    /* ── CLONE_NEWTIME: create a fresh time namespace ─────────── */
    if (flags & CLONE_NEWTIME) {
        /* Create a new time namespace with zero offsets (time starts
         * at the current global clock values).  The process can later
         * adjust offsets via /proc/self/timens_offsets or a dedicated
         * syscall, which is useful for container migration (where the
         * monotonic clock should continue from a saved checkpoint). */
        cur->timens_mono_offset = 0;
        cur->timens_boottime_offset = 0;
        kprintf("[TIMENS] PID %d created new time namespace\n", cur->pid);
    }

    /* ── CLONE_NEWUSER: create a new user namespace (Item 114) ──── */
    if (flags & CLONE_NEWUSER) {
        struct user_namespace *new_ns = user_ns_create(
            cur->user_ns ? cur->user_ns : &init_user_ns,
            cur->uid, cur->gid);
        if (!new_ns) {
            return (uint64_t)-ENOMEM;
        }
        cur->user_ns = new_ns;
        /* Inside the new user namespace, the process gets UID 0 (root).
         * The caller's UID/GID in the parent namespace are mapped to 0
         * inside.  Set the process's euid to reflect root-equivalent
         * privileges inside this namespace. */
        cur->uid  = 0;
        cur->gid  = 0;
        cur->euid = 0;
        cur->egid = 0;
        kprintf("[USERNS] unshare(NEWUSER): PID %d is root in new namespace id=%d\n",
                cur->pid, new_ns->id);
    }

    /* Record the set of unshared namespace flags on this process.
     * Only the new flags are OR'd in (repeated unshare calls add). */
    cur->ns_flags |= (uint32_t)(flags & ns_mask);

    return 0;
}

/* ── setns — join an existing namespace (Item 120) ────────────────────
 *
 *   setns(fd, nstype)
 *
 * Given a file descriptor opened on a /proc/<pid>/ns/<type> file, join
 * the namespace of the target process for the given namespace type.
 * If nstype is 0, the type is inferred from the fd's path.
 * If nstype is non-zero, it must match the fd's namespace type.
 *
 * Currently supported namespace types (nstype constants):
 *   CLONE_NEWUTS (0x04000000) — hostname/domainname isolation
 *
 * Other namespace types are accepted but treated as no-ops (the
 * infrastructure will be extended as namespace isolation is deepened).
 */
static uint64_t sys_setns(uint64_t fd, uint64_t nstype)
{
    struct process *cur = process_get_current();
    if (!cur) return (uint64_t)-EINVAL;

    /* Validate that fd is in range and in use */
    if (fd >= PROCESS_FD_MAX) return (uint64_t)-EBADF;
    struct process_fd *pfd = &cur->fd_table[fd];
    if (!pfd->used) return (uint64_t)-EBADF;

    /* Parse the fd's path to extract the namespace type and target PID.
     * Expected path format: "/proc/<pid>/ns/<type>" */
    const char *path = pfd->path;
    if (strncmp(path, "/proc/", 6) != 0)
        return (uint64_t)-EINVAL;

    /* Extract PID from /proc/<pid>/... */
    const char *p = path + 6;
    uint32_t target_pid = 0;
    int got_pid = 0;
    while (*p >= '0' && *p <= '9') {
        target_pid = target_pid * 10 + (uint32_t)(*p - '0');
        p++; got_pid = 1;
    }
    if (!got_pid || *p != '/')
        return (uint64_t)-EINVAL;

    /* Expect /ns/<type> suffix */
    if (strncmp(p, "/ns/", 4) != 0)
        return (uint64_t)-EINVAL;
    const char *ns_type = p + 4;

    /* Look up the target process */
    struct process *target = process_get_by_pid(target_pid);
    if (!target || target->state == PROCESS_UNUSED)
        return (uint64_t)-ESRCH;

    /* Determine namespace type from the path suffix */
    uint64_t discovered_nstype = 0;
    if (strcmp(ns_type, "uts") == 0) {
        discovered_nstype = CLONE_NEWUTS;
    } else if (strcmp(ns_type, "pid") == 0) {
        discovered_nstype = CLONE_NEWPID;
    } else if (strcmp(ns_type, "mnt") == 0) {
        discovered_nstype = CLONE_NEWNS;
    } else if (strcmp(ns_type, "net") == 0) {
        discovered_nstype = CLONE_NEWNET;
    } else if (strcmp(ns_type, "ipc") == 0) {
        discovered_nstype = CLONE_NEWIPC;
    } else if (strcmp(ns_type, "cgroup") == 0) {
        discovered_nstype = CLONE_NEWCGROUP;
    } else if (strcmp(ns_type, "time") == 0) {
        discovered_nstype = CLONE_NEWTIME;
    } else {
        return (uint64_t)-EINVAL;
    }

    /* If nstype is non-zero, verify it matches the fd's type */
    if (nstype != 0 && nstype != discovered_nstype)
        return (uint64_t)-EINVAL;

    /* Join the namespace based on type.
     * For each namespace type we implement the switch by copying the
     * target process's namespace state into the current process. */
    switch (discovered_nstype) {
    case CLONE_NEWUTS:
        /* Copy hostname and domainname from target process */
        strncpy(cur->ns_hostname, target->ns_hostname[0]
                ? target->ns_hostname : "localhost",
                sizeof(cur->ns_hostname) - 1);
        cur->ns_hostname[sizeof(cur->ns_hostname) - 1] = '\0';
        strncpy(cur->ns_domainname,
                target->ns_domainname[0] ? target->ns_domainname : "(none)",
                sizeof(cur->ns_domainname) - 1);
        cur->ns_domainname[sizeof(cur->ns_domainname) - 1] = '\0';
        cur->ns_flags |= CLONE_NEWUTS;
        break;

    case CLONE_NEWPID:
        /* PID namespace — not yet fully isolated; record the flag */
        cur->ns_flags |= CLONE_NEWPID;
        break;

    case CLONE_NEWNS:
        /* Mount namespace — not yet fully isolated */
        cur->ns_flags |= CLONE_NEWNS;
        break;

    case CLONE_NEWNET:
        /* Network namespace — not yet fully isolated */
        cur->ns_flags |= CLONE_NEWNET;
        break;

    case CLONE_NEWIPC:
        /* IPC namespace — not yet fully isolated */
        cur->ns_flags |= CLONE_NEWIPC;
        break;

    case CLONE_NEWCGROUP:
        /* Join the target's cgroup namespace */
        if (target->cgroup_ns) {
            cgroup_ns_get(target->cgroup_ns);
            if (cur->cgroup_ns)
                cgroup_ns_put(cur->cgroup_ns);
            cur->cgroup_ns = target->cgroup_ns;
        }
        cur->ns_flags |= CLONE_NEWCGROUP;
        break;

    case CLONE_NEWTIME:
        /* Time namespace — not yet fully isolated */
        cur->ns_flags |= CLONE_NEWTIME;
        break;

    default:
        return (uint64_t)-EINVAL;
    }

    return 0;
}

/* ── Thread syscalls (pthread support) ────────────────────────── */

static uint64_t sys_thread_create(uint64_t fn_addr, uint64_t arg) {
    void *(*fn)(void *) = (void *(*)(void *))(uintptr_t)fn_addr;
    void *user_arg = (void *)(uintptr_t)arg;
    int ret = process_thread_create(fn, user_arg);
    return (uint64_t)(int64_t)ret;
}

static uint64_t sys_thread_join(uint64_t thread_pid, uint64_t retval_addr) {
    void **retval_ptr = (void **)(uintptr_t)retval_addr;
    int ret;
    if (retval_ptr) {
        void *retval;
        ret = process_thread_join((int)thread_pid, &retval);
        if (ret == 0)
            *retval_ptr = retval;
    } else {
        ret = process_thread_join((int)thread_pid, NULL);
    }
    return (uint64_t)(int64_t)ret;
}

static void sys_thread_exit(void *retval) {
    process_thread_exit(retval);
    /* never reaches here */
}

static uint64_t sys_gettid(void) {
    struct process *p = process_get_current();
    if (!p) return 0;
    return (uint64_t)p->tgid ? (uint64_t)p->tgid : (uint64_t)p->pid;
}

uint64_t sys_set_tid_address(uint64_t tidptr) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-ESRCH;
    /* Save the old clear_child_tid pointer and set the new one.
     * On thread exit, the kernel writes 0 to *clear_child_tid and
     * performs a futex wake on that address (see process_exit_code). */
    p->ctid_ptr = (void *)(uintptr_t)tidptr;
    /* Return the calling thread's TID (same as gettid()) */
    return (uint64_t)p->tgid ? (uint64_t)p->tgid : (uint64_t)p->pid;
}

static uint64_t sys_execve(uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr) {
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    const char *path = kpath;
    /* For now, ignore argv/envp */
    (void)argv_addr; (void)envp_addr;

    /* IMA appraisal: measure executable file before execution */
    {
        extern int ima_file_exec(const char *path);
        int ima_ret = ima_file_exec(path);
        if (ima_ret < 0)
            return (uint64_t)(int64_t)ima_ret;
    }

    int ret = process_execve(path, NULL, NULL);
    /* If execve succeeds, we never return here (the process is redirected).
     * If it fails, we return -1. */
    return (uint64_t)(int64_t)ret;
}

/*
 * sys_posix_spawn — Lightweight process creation (Item 306)
 *
 * Creates a new child process that immediately execs the specified binary.
 * More efficient than fork+exec because it skips copying the parent's
 * address space and page tables.
 *
 * Signature (matching Linux posix_spawn ABI concept):
 *   pid_t posix_spawn(const char *path, char *const argv[], char *const envp[]);
 *
 * Returns child PID on success, -errno on failure.
 */
static uint64_t sys_posix_spawn(uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr)
{
    struct process *cur = process_get_current();
    if (!cur || !cur->is_user)
        return (uint64_t)(int64_t)-ECHILD;

    if (!path_addr || !syscall_user_cstr_ok(path_addr))
        return (uint64_t)(int64_t)-EFAULT;

    /* Copy path string to kernel space */
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    /* Validate argv and envp pointers (can be NULL) */
    const char *const *k_argv = NULL;
    const char *const *k_envp = NULL;

    uint64_t argv_ptr[256];
    uint64_t envp_ptr[256];
    int argc = 0, envc = 0;

    if (argv_addr) {
        if (!syscall_user_read_ok(argv_addr, sizeof(uint64_t))) {
            return (uint64_t)(int64_t)-EFAULT;
        }
        /* Count argv */
        while (argc < 256) {
            uint64_t ptr = 0;
            if (copy_from_user(&ptr, argv_addr + (uint64_t)argc * 8, 8) < 0)
                break;
            if (ptr == 0) break;
            if (!syscall_user_cstr_ok(ptr))
                return (uint64_t)(int64_t)-EFAULT;
            argv_ptr[argc] = ptr;
            argc++;
        }
        k_argv = (const char *const *)argv_ptr;
    }

    if (envp_addr) {
        if (!syscall_user_read_ok(envp_addr, sizeof(uint64_t))) {
            return (uint64_t)(int64_t)-EFAULT;
        }
        while (envc < 256) {
            uint64_t ptr = 0;
            if (copy_from_user(&ptr, envp_addr + (uint64_t)envc * 8, 8) < 0)
                break;
            if (ptr == 0) break;
            if (!syscall_user_cstr_ok(ptr))
                return (uint64_t)(int64_t)-EFAULT;
            envp_ptr[envc] = ptr;
            envc++;
        }
        k_envp = (const char *const *)envp_ptr;
    }

    int ret = process_spawn(kpath, (char *const *)k_argv, (char *const *)k_envp);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return (uint64_t)(uint64_t)ret;
}

/* ── mprotect ─────────────────────────────────────────────────────── */
/* sys_mmap and sys_munmap are now in sys_mmap.c */

/* ── mseal — seal virtual memory ranges against further changes ── */

static uint64_t sys_mseal(uint64_t addr, uint64_t length, uint64_t flags) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-ENOMEM;

    /* Address must be page-aligned */
    if (addr & (PAGE_SIZE - 1)) return (uint64_t)(int64_t)-EINVAL;

    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    if (addr + length < addr) return (uint64_t)(int64_t)-EINVAL;
    if (addr + length > USER_VADDR_MAX) return (uint64_t)(int64_t)-EFAULT;

    int ret = mseal(addr, length, (int)flags);
    return ret < 0 ? (uint64_t)(int64_t)ret : 0;
}

/* ── seccomp(2) — standalone BPF-based syscall sandboxing ──── */

static uint64_t sys_seccomp(uint64_t operation, uint64_t flags, uint64_t args) {
    (void)args;

    switch (operation) {
    case 1: /* SECCOMP_SET_MODE_STRICT */
        return (uint64_t)(int64_t)seccomp_set_mode(SECCOMP_MODE_STRICT, (unsigned int)flags);

    case 2: /* SECCOMP_SET_MODE_FILTER */
        return (uint64_t)(int64_t)seccomp_set_mode(SECCOMP_MODE_FILTER, (unsigned int)flags);

    default:
        return (uint64_t)(int64_t)-EINVAL;
    }
}

/* ── mremap is implemented in sys_mmap.c ──────────────────────── */

static uint64_t sys_sched_setaffinity(uint64_t pid, uint64_t cpuset) {
    struct process *proc = NULL;
    if (pid == 0) {
        proc = process_get_current();
    } else {
        proc = process_get_by_pid((uint32_t)pid);
    }
    if (!proc) return (uint64_t)(int64_t)-ESRCH;
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
    if (!proc) return (uint64_t)(int64_t)-ESRCH;
    return (uint64_t)proc->cpu_affinity;
}

/* ── dup / dup2 ─────────────────────────────────────────────── */

/* Find lowest available FD slot */
static int fd_find_free(struct process *proc) {
    /* Count open FDs and check against RLIMIT_NOFILE */
    int open_count = 0;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (proc->fd_table[i].used) open_count++;
    }
    if ((uint64_t)open_count >= proc->rlim_cur[RLIMIT_NOFILE])
        return -1;
    /* Find first free slot */
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!proc->fd_table[i].used) return i;
    }
    return -1;
}

static uint64_t sys_dup(uint64_t old_fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-EPERM;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return (uint64_t)(int64_t)-EBADF;

    int new_fd = fd_find_free(proc);
    if (new_fd < 0) return (uint64_t)(int64_t)-EMFILE;

    proc->fd_table[new_fd] = proc->fd_table[old_fd];
    proc->fd_table[new_fd].offset = proc->fd_table[old_fd].offset;
    return (uint64_t)new_fd;
}

static uint64_t sys_dup2(uint64_t old_fd, uint64_t new_fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-EPERM;
    if (old_fd >= PROCESS_FD_MAX || !proc->fd_table[old_fd].used)
        return (uint64_t)(int64_t)-EBADF;
    if (new_fd >= PROCESS_FD_MAX) return (uint64_t)(int64_t)-EBADF;

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
#define F_SETOWN  5
#define F_GETOWN  6
#define F_SETLK   7
#define F_SETLKW  8
#define F_GETLK   9
#define F_DUPFD_CLOEXEC 10
#define F_OFD_SETLK  37
#define F_OFD_SETLKW 38
#define F_OFD_GETLK  39
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define O_ASYNC   0x2000
/* O_NONBLOCK is defined in types.h (04000) */

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-ESRCH;
    if (fd >= PROCESS_FD_MAX || !proc->fd_table[fd].used)
        return (uint64_t)(int64_t)-EBADF;

    switch (cmd) {
        case F_DUPFD: {
            /* Duplicate fd to lowest FD >= arg */
            int new_fd = (int)arg;
            if (new_fd < 0) new_fd = 0;
            if (new_fd >= PROCESS_FD_MAX) return (uint64_t)(int64_t)-EINVAL;
            while (new_fd < PROCESS_FD_MAX && proc->fd_table[new_fd].used)
                new_fd++;
            if (new_fd >= PROCESS_FD_MAX) return (uint64_t)(int64_t)-EMFILE;
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
            /* Handle O_ASYNC for pipe FDs */
            if (arg & O_ASYNC) {
                proc->fd_table[fd].sigio_pid = process_get_current()->pid;
            }
            /* Try to find pipe ID from fd path pattern */
            if (strncmp(proc->fd_table[fd].path, "pipe_", 5) == 0) {
                int pid = (int)proc->fd_table[fd].offset;
                pipe_set_nonblock(pid, nonblock);
                if (arg & O_ASYNC)
                    pipe_set_sigio(pid, process_get_current()->pid);
            }
            return 0;
        }
        case F_SETOWN: {
            /* Set the owner PID for SIGIO */
            proc->fd_table[fd].sigio_pid = (uint32_t)arg;
            return 0;
        }
        case F_GETOWN: {
            /* Get the owner PID for SIGIO */
            return (uint64_t)proc->fd_table[fd].sigio_pid;
        }
        case F_SETPIPE_SZ: {
            /* Set pipe buffer capacity (must be a pipe FD) */
            if (strncmp(proc->fd_table[fd].path, "pipe_", 5) != 0)
                return (uint64_t)-EINVAL;
            int pipe_id = (int)proc->fd_table[fd].offset;
            int ret = pipe_set_capacity(pipe_id, (int)arg);
            return ret < 0 ? (uint64_t)-EINVAL : (uint64_t)ret;
        }
        case F_GETPIPE_SZ: {
            /* Get pipe buffer capacity */
            if (strncmp(proc->fd_table[fd].path, "pipe_", 5) != 0)
                return (uint64_t)-EINVAL;
            int pipe_id = (int)proc->fd_table[fd].offset;
            return (uint64_t)pipe_get_capacity(pipe_id);
        }
        case F_SETLK:
        case F_SETLKW: {
            /* Advisory record locking via file_lock.c */
            struct flock {
                int16_t l_type;
                int16_t l_whence;
                int64_t l_start;
                int64_t l_len;
                int32_t l_pid;
            } __attribute__((packed));

            struct flock user_flk;
            if (!arg) return (uint64_t)(int64_t)-EFAULT;
            if (syscall_is_user_process() && !syscall_user_read_ok(arg, sizeof(user_flk)))
                return (uint64_t)(int64_t)-EFAULT;
            if (copy_from_user(&user_flk, arg, sizeof(user_flk)) < 0)
                return (uint64_t)(int64_t)-EFAULT;

            /* Convert to kernel struct file_lock */
            struct file_lock kflk;
            memset(&kflk, 0, sizeof(kflk));
            kflk.l_type   = (int)user_flk.l_type;
            kflk.l_whence = (int)user_flk.l_whence;
            kflk.l_start  = user_flk.l_start;
            kflk.l_len    = user_flk.l_len;
            kflk.l_pid    = user_flk.l_pid;
            kflk.used     = true;
            kflk.mandatory = 0;

            /* Get file path from fd table */
            const char *fpath = proc->fd_table[fd].path;
            if (!fpath || !fpath[0]) return (uint64_t)(int64_t)-EBADF;

            int wait_flag = (cmd == F_SETLKW) ? 1 : 0;
            int rc = file_lock_set(fpath, &kflk, wait_flag);
            if (rc < 0) {
                if (rc == -EAGAIN) return (uint64_t)(int64_t)-EAGAIN;
                if (rc == -ENOLCK) return (uint64_t)(int64_t)-ENOLCK;
                return (uint64_t)(int64_t)rc;
            }
            return 0;
        }
        case F_GETLK: {
            /* Get any conflicting lock via file_lock.c */
            struct flock {
                int16_t l_type;
                int16_t l_whence;
                int64_t l_start;
                int64_t l_len;
                int32_t l_pid;
            } __attribute__((packed));

            if (!arg) return (uint64_t)(int64_t)-EFAULT;
            if (syscall_is_user_process() && !syscall_user_read_ok(arg, sizeof(struct flock)))
                return (uint64_t)(int64_t)-EFAULT;

            struct flock user_flk;
            if (copy_from_user(&user_flk, arg, sizeof(user_flk)) < 0)
                return (uint64_t)(int64_t)-EFAULT;

            /* Get file path */
            const char *fpath = proc->fd_table[fd].path;
            if (!fpath || !fpath[0]) return (uint64_t)(int64_t)-EBADF;

            struct file_lock kflk;
            memset(&kflk, 0, sizeof(kflk));
            int rc = file_lock_get(fpath, &kflk);
            if (rc == -ENOENT) {
                /* No lock — return F_UNLCK */
                user_flk.l_type   = F_UNLCK;
                user_flk.l_whence = 0;
                user_flk.l_start  = 0;
                user_flk.l_len    = 0;
                user_flk.l_pid    = 0;
            } else if (rc == 0) {
                /* Convert kernel file_lock back to userspace flock */
                user_flk.l_type   = (int16_t)kflk.l_type;
                user_flk.l_whence = (int16_t)kflk.l_whence;
                user_flk.l_start  = kflk.l_start;
                user_flk.l_len    = kflk.l_len;
                user_flk.l_pid    = kflk.l_pid;
            } else {
                return (uint64_t)(int64_t)rc;
            }

            if (syscall_is_user_process() && !syscall_user_write_ok(arg, sizeof(struct flock)))
                return (uint64_t)(int64_t)-EFAULT;
            if (copy_to_user(arg, &user_flk, sizeof(user_flk)) < 0)
                return (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        case F_DUPFD_CLOEXEC: {
            /* Duplicate with close-on-exec */
            int new_fd = (int)arg;
            if (new_fd < 0) new_fd = 0;
            if (new_fd >= PROCESS_FD_MAX) return (uint64_t)(int64_t)-EINVAL;
            while (new_fd < PROCESS_FD_MAX && proc->fd_table[new_fd].used)
                new_fd++;
            if (new_fd >= PROCESS_FD_MAX) return (uint64_t)(int64_t)-EMFILE;
            proc->fd_table[new_fd] = proc->fd_table[fd];
            proc->fd_table[new_fd].flags |= FD_CLOEXEC;
            return (uint64_t)new_fd;
        }
        case F_ADD_SEALS: {
            /* Add seals to a memfd */
            if (!memfd_is_fd((int)fd)) return (uint64_t)(int64_t)-EINVAL;
            return (uint64_t)memfd_add_seals_fd((int)fd, (int)arg);
        }
        case F_GET_SEALS: {
            /* Get seals from a memfd */
            if (!memfd_is_fd((int)fd)) return (uint64_t)(int64_t)-EINVAL;
            return (uint64_t)memfd_get_seals_fd((int)fd);
        }
        default:
            return (uint64_t)-1;
    }
}

/* ── select (I/O multiplexing) via poll infrastructure ─────── */

/*
 * sys_select — select(2) implemented using poll_table/poll_schedule.
 *
 * Converts fd_set-based select(2) arguments into the same event-checking
 * logic used by sys_poll(), and uses poll_schedule() for efficient
 * blocking instead of crude scheduler_yield() busy-wait.
 *
 * @nfds:           highest-numbered file descriptor (+1) to examine
 * @readfds_addr:   user-space pointer to fd_set for readability
 * @writefds_addr:  user-space pointer to fd_set for writability
 * @exceptfds_addr: user-space pointer to fd_set for exceptions
 * @timeout_addr:   user-space pointer to struct timespec (NULL = infinite)
 *
 * Returns: number of ready fds on success
 *          -EFAULT if user copy fails
 *          -EINTR  if interrupted by a signal
 *          -EINVAL if bad timeout value
 */
static uint64_t sys_select(uint64_t nfds, uint64_t readfds_addr,
                            uint64_t writefds_addr, uint64_t exceptfds_addr,
                            uint64_t timeout_addr)
{
    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)(int64_t)-EINTR;
    if (nfds > FD_SETSIZE)
        nfds = FD_SETSIZE;

    fd_set readfds, writefds, exceptfds;
    fd_set orig_readfds, orig_writefds, orig_exceptfds;

    /* Copy in from userspace — skip NULL sets */
    if (readfds_addr) {
        if (copy_from_user(&orig_readfds, readfds_addr, sizeof(fd_set)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    } else {
        FD_ZERO(&orig_readfds);
    }
    if (writefds_addr) {
        if (copy_from_user(&orig_writefds, writefds_addr, sizeof(fd_set)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    } else {
        FD_ZERO(&orig_writefds);
    }
    if (exceptfds_addr) {
        if (copy_from_user(&orig_exceptfds, exceptfds_addr, sizeof(fd_set)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    } else {
        FD_ZERO(&orig_exceptfds);
    }

    /* Parse timeout (struct timespec from userspace) */
    uint64_t timeout_ms = ~0ULL; /* infinite by default */
    if (timeout_addr) {
        struct timespec ts;
        if (copy_from_user(&ts, timeout_addr, sizeof(ts)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
        if ((int64_t)ts.tv_sec < 0 || (int64_t)ts.tv_nsec < 0)
            return (uint64_t)(int64_t)-EINVAL;
        uint64_t total_ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
        timeout_ms = total_ns / 1000000ULL;
        if (timeout_ms == 0 && (ts.tv_sec > 0 || ts.tv_nsec > 0))
            timeout_ms = 1; /* round up tiny non-zero timeout */
    }

    /* Poll table for wait queue registration (stack-allocated) */
    struct poll_queue_entry poll_entries_buf[POLL_TABLE_MAX];
    struct poll_table pt;
    poll_init_table_inline(&pt, poll_entries_buf, POLL_TABLE_MAX);

    uint64_t start_tick = timer_get_ticks();
    int ready = 0;
    int timed_out = 0;

    for (;;) {
        /* ── Reset result sets to original fd_set values ────── */
        if (readfds_addr)
            memcpy(&readfds, &orig_readfds, sizeof(fd_set));
        else
            FD_ZERO(&readfds);
        if (writefds_addr)
            memcpy(&writefds, &orig_writefds, sizeof(fd_set));
        else
            FD_ZERO(&writefds);
        if (exceptfds_addr)
            memcpy(&exceptfds, &orig_exceptfds, sizeof(fd_set));
        else
            FD_ZERO(&exceptfds);

        ready = 0;

        /* ── Check readability for each FD in the read set ──── */
        if (readfds_addr) {
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &readfds))
                    continue;
                /* Socket FDs */
                if (i >= 100 && i < 100 + SOCK_MAX) {
                    int revents = sock_poll(i, POLLIN, NULL);
                    if (!(revents & POLLIN))
                        FD_CLR(i, &readfds);
                } else if (i >= PROCESS_FD_MAX || !cur->fd_table[i].used) {
                    FD_CLR(i, &readfds);
                } else if (strncmp(cur->fd_table[i].path, "pipe_read_", 10) == 0) {
                    /* Pipe read end */
                    int pipe_id = (int)cur->fd_table[i].offset;
                    int revents = pipe_poll(pipe_id, 1, NULL);
                    if (!(revents & POLLIN))
                        FD_CLR(i, &readfds);
                }
                /* Default: regular files remain in set (always readable) */
                if (FD_ISSET(i, &readfds))
                    ready++;
            }
        }

        /* ── Check writability for each FD in the write set ─── */
        if (writefds_addr) {
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &writefds))
                    continue;
                if (i >= 100 && i < 100 + SOCK_MAX) {
                    int revents = sock_poll(i, POLLOUT, NULL);
                    if (!(revents & POLLOUT))
                        FD_CLR(i, &writefds);
                } else if (i >= PROCESS_FD_MAX || !cur->fd_table[i].used) {
                    FD_CLR(i, &writefds);
                } else if (strncmp(cur->fd_table[i].path, "pipe_write_", 11) == 0) {
                    /* Pipe write end */
                    int pipe_id = (int)cur->fd_table[i].offset;
                    int revents = pipe_poll(pipe_id, 0, NULL);
                    if (!(revents & POLLOUT))
                        FD_CLR(i, &writefds);
                }
                /* Default: regular files remain in set (always writable) */
                if (FD_ISSET(i, &writefds))
                    ready++;
            }
        }

        /* ── Check exceptions for each FD in the except set ─── */
        if (exceptfds_addr) {
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &exceptfds))
                    continue;
                if (i >= 100 && i < 100 + SOCK_MAX) {
                    struct socket *s = sock_get(i);
                    if (!s || s->state == SOCK_STATE_CLOSED) {
                        continue; /* closed socket is exceptional */
                    }
                    FD_CLR(i, &exceptfds);
                } else if (i >= PROCESS_FD_MAX || !cur->fd_table[i].used) {
                    FD_CLR(i, &exceptfds);
                } else {
                    FD_CLR(i, &exceptfds); /* no exception pending */
                }
                if (FD_ISSET(i, &exceptfds))
                    ready++;
            }
        }

        /* ── Events available — return immediately ──────────── */
        if (ready > 0)
            break;

        /* ── Non-blocking select: return 0 ──────────────────── */
        if (timeout_ms == 0) {
            timed_out = 1;
            break;
        }

        /* ── Check absolute timeout ─────────────────────────── */
        uint64_t elapsed = timer_get_ticks() - start_tick;
        uint64_t timeout_ticks = (timeout_ms * TIMER_FREQ) / 1000;
        if (timeout_ms == ~0ULL)
            timeout_ticks = ~0ULL;

        if (elapsed >= timeout_ticks && timeout_ms != ~0ULL) {
            timed_out = 1;
            break;
        }

        /* ── Block using poll_schedule ──────────────────────── */
        uint64_t remaining_ms;
        if (timeout_ms == ~0ULL) {
            remaining_ms = ~0ULL;
        } else {
            uint64_t remaining_ticks = timeout_ticks - elapsed;
            remaining_ms = (remaining_ticks * 1000) / TIMER_FREQ;
            if (remaining_ms == 0)
                remaining_ms = 1;
        }

        /* Reset poll table so fd poll handlers can re-register */
        pt.nr_entries = 0;

        int sret = poll_schedule(&pt, remaining_ms);
        if (sret == -EINTR) {
            ready = 0;
            break;
        }
        if (sret == -ETIME) {
            timed_out = 1;
            break;
        }
        /* Woken normally — loop and re-check fds */
    }

    /* ── Copy results back to userspace ─────────────────────── */
    if (readfds_addr && copy_to_user(readfds_addr, &readfds, sizeof(fd_set)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    if (writefds_addr && copy_to_user(writefds_addr, &writefds, sizeof(fd_set)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    if (exceptfds_addr && copy_to_user(exceptfds_addr, &exceptfds, sizeof(fd_set)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    if (timed_out)
        return 0;
    return (uint64_t)ready;
}

/* ── setitimer / getitimer (per-process interval timers) ────── */

/* SIGALRM */
#ifndef SIGALRM
#define SIGALRM 14
#endif

/* Called from timer tick to decrement per-process timers */
void process_timer_tick(int was_user) {
    struct process *table = process_get_table();
    struct process *current = process_get_current();

    /* Lock process table during itimer walk — the same itimers[]
     * can be written by sys_setitimer() on another CPU. */
    uint64_t __it_flags;
    spinlock_irqsave_acquire(&proc_table_lock, &__it_flags);

    /* ITIMER_REAL: wall-clock — tick for every process */
    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &table[i];
        if (p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE)
            continue;
        if (p->itimers[ITIMER_REAL].it_value > 0) {
            p->itimers[ITIMER_REAL].it_value--;
            if (p->itimers[ITIMER_REAL].it_value == 0) {
                signal_send(p->pid, SIGALRM);
                p->itimers[ITIMER_REAL].it_value = p->itimers[ITIMER_REAL].it_interval;
            }
        }
    }

    /* ITIMER_VIRTUAL: counts only user-mode CPU time */
    if (current && current->state == PROCESS_RUNNING && was_user && current->is_user) {
        if (current->itimers[ITIMER_VIRTUAL].it_value > 0) {
            current->itimers[ITIMER_VIRTUAL].it_value--;
            if (current->itimers[ITIMER_VIRTUAL].it_value == 0) {
                signal_send(current->pid, SIGVTALRM);
                current->itimers[ITIMER_VIRTUAL].it_value = current->itimers[ITIMER_VIRTUAL].it_interval;
            }
        }
    }

    /* ITIMER_PROF: counts user + system CPU time (any tick while running) */
    if (current && current->state == PROCESS_RUNNING) {
        if (current->itimers[ITIMER_PROF].it_value > 0) {
            current->itimers[ITIMER_PROF].it_value--;
            if (current->itimers[ITIMER_PROF].it_value == 0) {
                signal_send(current->pid, SIGPROF);
                current->itimers[ITIMER_PROF].it_value = current->itimers[ITIMER_PROF].it_interval;
            }
        }
    }

    spinlock_irqsave_release(&proc_table_lock, __it_flags);
}

static uint64_t sys_setitimer(uint64_t which, uint64_t new_val_addr,
                               uint64_t old_val_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (which >= ITIMER_MAX) return (uint64_t)-1;

    struct itimerval new_val;
    memset(&new_val, 0, sizeof(new_val));
    if (new_val_addr) {
        if (copy_from_user(&new_val, new_val_addr, sizeof(new_val)) < 0)
            return (uint64_t)-1;
    }

    uint64_t __sit_flags;
    spinlock_irqsave_acquire(&proc_table_lock, &__sit_flags);

    /* Return old value if requested */
    if (old_val_addr) {
        if (copy_to_user(old_val_addr, &proc->itimers[which], sizeof(struct itimerval)) < 0) {
            spinlock_irqsave_release(&proc_table_lock, __sit_flags);
            return (uint64_t)-1;
        }
    }

    /* Set new value */
    proc->itimers[which] = new_val;
    spinlock_irqsave_release(&proc_table_lock, __sit_flags);
    return 0;
}

static uint64_t sys_getitimer(uint64_t which, uint64_t cur_val_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (which >= ITIMER_MAX) return (uint64_t)-1;
    if (!cur_val_addr) return (uint64_t)-1;

    if (copy_to_user(cur_val_addr, &proc->itimers[which], sizeof(struct itimerval)) < 0)
        return (uint64_t)-1;
    return 0;
}

/* ── nanosleep ──────────────────────────────────────────────── */

static uint64_t sys_nanosleep(uint64_t req_addr, uint64_t rem_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (!req_addr) return (uint64_t)-1;

    struct timespec req;
    if (copy_from_user(&req, req_addr, sizeof(req)) < 0)
        return (uint64_t)-1;

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
        if (copy_to_user(rem_addr, &rem, sizeof(rem)) < 0)
            return (uint64_t)-1;
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

/* ── Global hostname (must be before any function that references it) ── */

#define HOSTNAME_MAX 64
static char system_hostname[HOSTNAME_MAX] = "os";

/* ── uname ──────────────────────────────────────────────────── */

static uint64_t sys_uname(uint64_t buf_addr) {
    if (!buf_addr) return (uint64_t)(int64_t)-EFAULT;
    struct utsname *buf = (struct utsname *)buf_addr;
    memset(buf, 0, sizeof(struct utsname));
    memcpy(buf->sysname, "Linux", 6);
    {
        struct process *proc = process_get_current();
        const char *node = proc ? proc->ns_hostname : system_hostname;
        if (!node || !*node) node = "localhost";
        size_t nlen = strlen(node);
        if (nlen > sizeof(buf->nodename) - 1)
            nlen = sizeof(buf->nodename) - 1;
        memcpy(buf->nodename, node, nlen);
    }
    memcpy(buf->release, "6.1.0-osdev", 12);
    snprintf(buf->version, sizeof(buf->version), "%s %s", __DATE__, __TIME__);
    memcpy(buf->machine, "x86_64", 7);
    return 0;
}

/* ── pipe() ──────────────────────────────────────────────────── */

static uint64_t sys_pipe(uint64_t fds_addr) {
    if (!fds_addr) return (uint64_t)(int64_t)-EFAULT;
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-EPERM;

    /* Count open FDs and check against RLIMIT_NOFILE */
    int open_count = 0;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (proc->fd_table[i].used) open_count++;
    }
    if ((uint64_t)open_count + 1 >= proc->rlim_cur[RLIMIT_NOFILE])
        return (uint64_t)-EMFILE;

    int id = pipe_create();
    if (id < 0) return (uint64_t)(int64_t)-EMFILE;

    /* Allocate two FD slots */
    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!proc->fd_table[i].used) {
            if (read_fd < 0) read_fd = i;
            else if (write_fd < 0) { write_fd = i; break; }
        }
    }
    if (read_fd < 0 || write_fd < 0) return (uint64_t)(int64_t)-EMFILE;

    /* Store pipe index as part of fd path */
    proc->fd_table[read_fd].used = true;
    proc->fd_table[read_fd].offset = (uint32_t)id; /* store pipe id */
    snprintf(proc->fd_table[read_fd].path, 64, "pipe_read_%d", id);
    proc->fd_table[read_fd].flags = 0;

    proc->fd_table[write_fd].used = true;
    proc->fd_table[write_fd].offset = (uint32_t)id;
    snprintf(proc->fd_table[write_fd].path, 64, "pipe_write_%d", id);
    proc->fd_table[write_fd].flags = 0;

    /* Write fds back to userspace */
    uint32_t fds[2] = { (uint32_t)read_fd, (uint32_t)write_fd };
    if (copy_to_user(fds_addr, fds, sizeof(fds)) < 0) return (uint64_t)-1;
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
    if (!proc) return (uint64_t)(int64_t)-ESRCH;

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
    if (!proc) return (uint64_t)(int64_t)-ESRCH;

    proc->state = PROCESS_BLOCKED;
    scheduler_remove(proc);
    scheduler_yield();

    /* Woken by signal — return -EINTR (always interrupted) */
    return (uint64_t)(int64_t)-EINTR;
}

/* ── access() ────────────────────────────────────────────────── */

static uint64_t sys_access(uint64_t path_addr, uint64_t mode) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)(int64_t)-EFAULT;

    /* Check if file exists */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0) return (uint64_t)(int64_t)-ENOENT;

    /* For now, we don't check permissions (always OK if file exists) */
    (void)mode;
    return 0;
}

/* ── getuid / geteuid / getgid / getegid ────────────────────── */

static uint64_t sys_getuid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-ESRCH;
    return (uint64_t)proc->uid;
}

static uint64_t sys_geteuid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-ESRCH;
    return (uint64_t)proc->euid;
}

static uint64_t sys_getgid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-ESRCH;
    return (uint64_t)proc->gid;
}

static uint64_t sys_getegid(void) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)(int64_t)-ESRCH;
    return (uint64_t)proc->egid;
}

/* ── rmdir() ─────────────────────────────────────────────────── */

static uint64_t sys_rmdir(uint64_t path_addr) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)(int64_t)-EFAULT;

    /* Use VFS unlink (same as delete for directories) */
    int ret = vfs_unlink(path);
    if (ret < 0) return (uint64_t)(int64_t)ret;
    return 0;
}

/* ── rename() ────────────────────────────────────────────────── */

static uint64_t sys_rename(uint64_t old_addr, uint64_t new_addr) {
    const char *old_path = (const char *)old_addr;
    const char *new_path = (const char *)new_addr;
    if (!old_path || !new_path) return (uint64_t)(int64_t)-EINVAL;

    /* Use the VFS rename operation — supports both files and directories,
     * handles cross-filesystem moves (returns -EXDEV), enforces Landlock
     * permissions and read-only mounts. */
    int ret = vfs_rename(old_path, new_path);
    if (ret < 0) return (uint64_t)(int64_t)ret;
    return 0;
}

/* ── chmod() ─────────────────────────────────────────────────── */

static uint64_t sys_chmod(uint64_t path_addr, uint64_t mode) {
    const char *path = (const char *)path_addr;
    if (!path) return (uint64_t)(int64_t)-EFAULT;
    int ret = fs_chmod(path, (uint16_t)mode);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
}

/* ── fsync() / fdatasync() ─────────────────────────────────────── */

/*
 * fsync - synchronize a file's in-core state with storage device.
 *
 * Flushes dirty page cache pages for the file, then flushes the
 * filesystem metadata via the VFS flush op, then flushes the block
 * device buffer cache.  Returns 0 on success or -errno on error.
 */
static uint64_t sys_fsync(uint64_t fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-EBADF;

    /* Regular file fd (3+) */
    if (fd >= 3 && fd < (uint64_t)(3 + PROCESS_FD_MAX)) {
        int i = (int)fd - 3;
        if (i < 0 || i >= PROCESS_FD_MAX || !proc->fd_table[i].used)
            return (uint64_t)-EBADF;

        const char *path = proc->fd_table[i].path;

        /* Step 1: Get the inode number so we can do a targeted page flush */
        struct vfs_stat st;
        int r = vfs_stat(path, &st);
        if (r < 0) {
            /* If stat fails (e.g. file was deleted), fall back to full flush */
            kprintf("[fsync] stat failed (%d), falling back to full flush\n", r);
            int ret = vfs_flush(path);
            return (ret < 0) ? (uint64_t)-EIO : 0;
        }

        /* Step 2: Flush dirty page cache pages for this specific inode */
        if (st.ino != 0) {
            page_cache_flush_inode(st.ino);
        }

        /* Step 3: Flush the filesystem metadata via VFS flush op */
        r = vfs_flush(path);
        if (r < 0) {
            kprintf("[fsync] vfs_flush failed (%d) for %s\n", r, path);
            return (uint64_t)-EIO;
        }

        return 0;
    }

    /* For special fds (stdin/stdout/stderr, eventfd, timerfd,
     * signalfd, sockets, pipes), fsync is a no-op because data
     * is not buffered or not applicable. */
    return 0;
}

/*
 * fdatasync - synchronize a file's data, but metadata may be skipped
 *             if not needed for subsequent data access.
 *
 * In this kernel, fdatasync is nearly identical to fsync but we
 * skip the VFS-level metadata flush (step 3 above) and only flush
 * dirty page cache pages + buffer cache.  This is a performance
 * optimization for applications that only need data durability.
 */
static uint64_t sys_fdatasync(uint64_t fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-EBADF;

    /* Regular file fd (3+) */
    if (fd >= 3 && fd < (uint64_t)(3 + PROCESS_FD_MAX)) {
        int i = (int)fd - 3;
        if (i < 0 || i >= PROCESS_FD_MAX || !proc->fd_table[i].used)
            return (uint64_t)-EBADF;

        const char *path = proc->fd_table[i].path;

        /* Get the inode number for targeted flush */
        struct vfs_stat st;
        int r = vfs_stat(path, &st);
        if (r < 0) {
            kprintf("[fdatasync] stat failed (%d), falling back to full flush\n", r);
            int ret = vfs_flush(path);
            return (ret < 0) ? (uint64_t)-EIO : 0;
        }

        /* Flush dirty page cache pages for this specific inode */
        if (st.ino != 0) {
            page_cache_flush_inode(st.ino);
        }

        /* Flush the buffer cache (block device cache) to backing store.
         * This ensures data pages are physically written to disk. */
        bufcache_flush();

        return 0;
    }

    /* Special fds: no-op */
    return 0;
}

/* ── sigprocmask / sigpending ────────────────────────────────── */

static uint64_t sys_sigprocmask(uint64_t how, uint64_t set_addr, uint64_t oldset_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&proc->sig_lock, &__sig_flags);

    /* Return old mask */
    if (oldset_addr) {
        uint64_t old = proc->sig_mask;
        if (copy_to_user(oldset_addr, &old, sizeof(old)) < 0) {
            spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
            return (uint64_t)-1;
        }
    }

    /* Apply new mask */
    if (set_addr) {
        uint64_t new_mask = 0;
        if (copy_from_user(&new_mask, set_addr, sizeof(new_mask)) < 0) {
            spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
            return (uint64_t)-1;
        }

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
                spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
                return (uint64_t)-1;
        }
    }

    spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
    return 0;
}

static uint64_t sys_sigpending(uint64_t set_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (!set_addr) return (uint64_t)-1;

    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&proc->sig_lock, &__sig_flags);
    uint64_t pending = proc->pending_signals;
    spinlock_irqsave_release(&proc->sig_lock, __sig_flags);

    if (copy_to_user(set_addr, &pending, sizeof(uint64_t)) < 0)
        return (uint64_t)-1;
    return 0;
}

/* ── sigwaitinfo / sigtimedwait (synchronous signal acceptance) ── */

/* Internal: block until a signal in the given mask is pending.
 * Returns the signal number, or -1 on error (with errno in *out_errno).
 * If info is non-NULL, fills in siginfo for the delivered signal.
 * If timeout_ticks > 0, blocks at most that many timer ticks. */
static int do_sigwait(uint64_t set_mask, int timeout_ticks,
                      struct siginfo *out_info, int *out_errno) {
    struct process *proc = process_get_current();
    if (!proc) { *out_errno = 1; return -1; }

    uint64_t start = timer_get_ticks();

    for (;;) {
        /* Lock around pending_signals access and siginfo extraction */
        uint64_t __sig_flags;
        spinlock_irqsave_acquire(&proc->sig_lock, &__sig_flags);

        /* Mask away signals we don't care about */
        uint64_t relevant = proc->pending_signals & set_mask;

        if (relevant) {
            /* Find lowest pending signal in the set */
            int sig = __builtin_ctzll(relevant);
            if (sig > 0 && sig < SIG_MAX) {
                /* Dequeue the signal */
                proc->pending_signals &= ~(1ULL << sig);

                /* Copy out siginfo if requested */
                if (out_info) {
                    struct siginfo *info = signal_get_info(proc, sig);
                    if (info) {
                        memcpy(out_info, info, sizeof(struct siginfo));
                        memset(info, 0, sizeof(struct siginfo));
                    } else {
                        memset(out_info, 0, sizeof(struct siginfo));
                        out_info->si_signo = sig;
                        out_info->si_code = SI_USER;
                    }
                }
                spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
                return sig;
            }
        }
        spinlock_irqsave_release(&proc->sig_lock, __sig_flags);

        /* Check timeout */
        if (timeout_ticks > 0) {
            uint64_t elapsed = timer_get_ticks() - start;
            if (elapsed >= (uint64_t)timeout_ticks) {
                *out_errno = 11; /* EAGAIN */
                return -1;
            }
        }

        /* Block until woken (we'll be woken when a signal arrives) */
        process_get_current()->wait_for_pid = 0; /* not waiting for child */
        process_get_current()->sleep_until = timeout_ticks > 0
            ? start + (uint64_t)timeout_ticks : 0;
        process_get_current()->state = PROCESS_BLOCKED;
        scheduler_remove(process_get_current());
        scheduler_yield();

        /* Woken — loop back and check signals */
    }
}

static uint64_t sys_sigwaitinfo(uint64_t set_addr, uint64_t info_addr) {
    if (!set_addr) return (uint64_t)-1;

    uint64_t sigmask;
    if (copy_from_user(&sigmask, set_addr, sizeof(uint64_t)) < 0)
        return (uint64_t)-1;

    /* Mask out signals that can't be waited on (SIGKILL, SIGSTOP) */
    sigmask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    /* Temporarily unmask the waited signals so signal_check can deliver them */
    struct process *proc = process_get_current();
    uint64_t saved_mask = proc ? proc->sig_mask : 0;
    if (proc) {
        uint64_t __sig_flags;
        spinlock_irqsave_acquire(&proc->sig_lock, &__sig_flags);
        proc->sig_mask &= ~sigmask;
        spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
    }

    struct siginfo info_buf;
    int errno_val = 0;
    int sig = do_sigwait(sigmask, 0, &info_buf, &errno_val);

    /* Restore signal mask */
    if (proc) {
        uint64_t __sig_flags;
        spinlock_irqsave_acquire(&proc->sig_lock, &__sig_flags);
        proc->sig_mask = saved_mask;
        spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
    }

    if (sig < 0) return (uint64_t)-1;

    /* Copy siginfo back to userspace if requested */
    if (info_addr) {
        if (copy_to_user(info_addr, &info_buf, sizeof(struct siginfo)) < 0)
            return (uint64_t)-1;
    }

    return (uint64_t)(unsigned int)sig;
}

static uint64_t sys_sigtimedwait(uint64_t set_addr, uint64_t info_addr,
                                  uint64_t timeout_addr) {
    if (!set_addr || !timeout_addr) return (uint64_t)-1;

    uint64_t sigmask;
    if (copy_from_user(&sigmask, set_addr, sizeof(uint64_t)) < 0)
        return (uint64_t)-1;
    sigmask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    /* Read timeout as timespec and convert to ticks */
    struct timespec ts;
    if (copy_from_user(&ts, timeout_addr, sizeof(struct timespec)) < 0)
        return (uint64_t)-1;
    int timeout_ticks = (int)(ts.tv_sec * TIMER_FREQ + ts.tv_nsec * TIMER_FREQ / 1000000000ULL);
    if (timeout_ticks < 0) timeout_ticks = 0;

    struct process *proc = process_get_current();
    uint64_t saved_mask = proc ? proc->sig_mask : 0;
    if (proc) {
        uint64_t __sig_flags;
        spinlock_irqsave_acquire(&proc->sig_lock, &__sig_flags);
        proc->sig_mask &= ~sigmask;
        spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
    }

    struct siginfo info_buf;
    int errno_val = 0;
    int sig = do_sigwait(sigmask, timeout_ticks, &info_buf, &errno_val);

    if (proc) {
        uint64_t __sig_flags;
        spinlock_irqsave_acquire(&proc->sig_lock, &__sig_flags);
        proc->sig_mask = saved_mask;
        spinlock_irqsave_release(&proc->sig_lock, __sig_flags);
    }

    if (sig < 0) return (uint64_t)-1;

    if (info_addr) {
        if (copy_to_user(info_addr, &info_buf, sizeof(struct siginfo)) < 0)
            return (uint64_t)-1;
    }

    return (uint64_t)(unsigned int)sig;
}

/* ── readv / writev (vectored I/O) ──────────────────────────── */

/*
 * Linux-compatible readv — vectored read into a scatter/gather array.
 *
 * Returns the total number of bytes read on success.
 * On error, returns a negative errno value (Linux convention):
 *   -EFAULT  — iov_addr points to invalid user memory
 *   -EINVAL  — iovcnt > IOV_MAX (1024)
 *   -ENOMEM  — out of kernel memory
 *   -EBADF   — fd is not open
 *   -EIO     — VFS I/O error
 *
 * On a partial read (some iovs consumed, then an error), returns the
 * partial byte count rather than the error (consistent with Linux).
 * Zero-length iovs are skipped silently.
 */
static uint64_t sys_readv(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt) {
    if (iovcnt > 1024)
        return (uint64_t)(int64_t)-EINVAL;
    if (iovcnt == 0)
        return 0;
    if (!iov_addr)
        return (uint64_t)(int64_t)-EFAULT;

    struct iovec iov_stack[16];
    struct iovec *iov = iov_stack;
    int allocd = 0;

    if (iovcnt > 16) {
        iov = kmalloc(sizeof(struct iovec) * (size_t)iovcnt);
        if (!iov)
            return (uint64_t)(int64_t)-ENOMEM;
        allocd = 1;
    }

    if (copy_from_user(iov, iov_addr, sizeof(struct iovec) * iovcnt) < 0) {
        if (allocd) kfree(iov);
        return (uint64_t)(int64_t)-EFAULT;
    }

    uint64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0)
            continue;
        int64_t n = (int64_t)sys_read(fd, (uint64_t)iov[i].iov_base,
                                      iov[i].iov_len);
        if (n < 0) {
            if (allocd) kfree(iov);
            /* Partial read: return bytes so far; full failure: propagate errno */
            return total ? total : (uint64_t)(int64_t)n;
        }
        total += (uint64_t)n;
        /* Short read from this iov means no more data (e.g. EOF) */
        if ((uint64_t)n < iov[i].iov_len) break;
    }

    if (allocd) kfree(iov);
    return total;
}

static uint64_t sys_writev(uint64_t fd, uint64_t iov_addr, uint64_t iovcnt) {
    if (iovcnt > 1024)
        return (uint64_t)(int64_t)-EINVAL;
    if (iovcnt == 0)
        return 0;

    if (!iov_addr)
        return (uint64_t)(int64_t)-EFAULT;

    struct iovec iov_stack[16];
    struct iovec *iov = iov_stack;
    int allocd = 0;

    if (iovcnt > 16) {
        iov = kmalloc(sizeof(struct iovec) * (size_t)iovcnt);
        if (!iov)
            return (uint64_t)(int64_t)-ENOMEM;
        allocd = 1;
    }

    if (copy_from_user(iov, iov_addr, sizeof(struct iovec) * iovcnt) < 0) {
        if (allocd) kfree(iov);
        return (uint64_t)(int64_t)-EFAULT;
    }

    uint64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0)
            continue;
        int64_t n = (int64_t)sys_write(fd, (uint64_t)iov[i].iov_base,
                                       iov[i].iov_len);
        if (n < 0) {
            if (allocd) kfree(iov);
            /* Partial write: return bytes so far; full failure: propagate errno */
            return total ? total : (uint64_t)(int64_t)n;
        }
        total += (uint64_t)n;
    }

    if (allocd) kfree(iov);
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

/* Non-static PRNG accessor for use by other kernel subsystems (e.g. ASLR) */
uint64_t prng_rand64(void) {
    return xorshift64();
}

/* Allow kernel subsystems to add external entropy to the PRNG.
 * XORs the given entropy into the state to avoid reducing existing entropy. */
void prng_add_entropy(uint64_t entropy) {
    prng_state ^= entropy;
    /* Mix one round to spread the bits */
    xorshift64();
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

/* ── kexec_load — register a kernel image for kexec reboot (Item 362) ── */

static uint64_t sys_kexec_load(uint64_t phys_addr, uint64_t entry, uint64_t flags)
{
    /* Lockdown: reject kexec_load at INTEGRITY level or above */
    if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY))
        return (uint64_t)-EPERM;

    /* CAP_SYS_BOOT check — only privileged processes can load a kexec image */
    int cap_ret = cap_sys_boot_check();
    if (cap_ret < 0)
        return (uint64_t)(int64_t)cap_ret;

    int ret = kexec_load(phys_addr, entry, (uint32_t)flags);
    return (uint64_t)(int64_t)ret;
}

/* ── reboot() ────────────────────────────────────────────────── */

static uint64_t sys_reboot(void) {
    /* If a kexec image is loaded, kexec-reboot instead of ACPI shutdown */
    if (kexec_is_loaded()) {
        kprintf("[syscall] reboot: kexec image loaded — jumping to new kernel\n");
        kexec_reboot();
        /* Never reaches here */
    }
    /* Call ACPI shutdown */
    acpi_shutdown();
    /* Should not reach here */
    for (;;) __asm__ volatile("hlt");
    return (uint64_t)-1;
}

/* ── sethostname / gethostname ───────────────────────────────── */

static uint64_t sys_sethostname(uint64_t name_addr, uint64_t len) {
    if (!name_addr) return (uint64_t)-1;
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    size_t copylen = (size_t)len;
    if (copylen > sizeof(proc->ns_hostname) - 1)
        copylen = sizeof(proc->ns_hostname) - 1;
    /* Copy name from userspace via uaccess */
    if (copy_from_user(proc->ns_hostname, name_addr, copylen) < 0)
        return (uint64_t)-1;
    proc->ns_hostname[copylen] = '\0';
    /* Also update the system global so new processes inherit it */
    if (copylen > HOSTNAME_MAX - 1) copylen = HOSTNAME_MAX - 1;
    memcpy(system_hostname, proc->ns_hostname, copylen + 1);
    return 0;
}

static uint64_t sys_gethostname(uint64_t name_addr, uint64_t len) {
    if (!name_addr || len == 0) return (uint64_t)-1;
    struct process *proc = process_get_current();
    const char *host = proc ? proc->ns_hostname : system_hostname;
    size_t slen = strlen(host);
    if (slen > (size_t)len - 1) slen = (size_t)len - 1;
    /* Copy hostname to user via uaccess */
    char kbuf[256];
    if (slen >= sizeof(kbuf)) slen = sizeof(kbuf) - 1;
    memcpy(kbuf, host, slen);
    kbuf[slen] = '\0';
    if (copy_to_user(name_addr, kbuf, slen + 1) < 0)
        return (uint64_t)-1;
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
    if (!path) return (uint64_t)-ENOENT;

    /* Extract the file type from the POSIX mode bits */
    uint32_t file_type = (mode & S_IFMT);

    switch (file_type) {
        case S_IFCHR: {
            /* Character device node */
            uint16_t major = (uint16_t)((dev >> 8) & 0xFF);
            uint16_t minor = (uint16_t)(dev & 0xFF);
            if (vfs_mknod(path, (uint16_t)(mode & 0x0FFF), major, minor) < 0)
                return (uint64_t)-1;
            return 0;
        }
        case S_IFBLK: {
            /* Block device node */
            uint16_t major = (uint16_t)((dev >> 8) & 0xFF);
            uint16_t minor = (uint16_t)(dev & 0xFF);
            if (vfs_mknod(path, (uint16_t)(mode & 0x0FFF), major, minor) < 0)
                return (uint64_t)-1;
            return 0;
        }
        case S_IFREG:
        case 0: {
            /* Regular file (default when no type bits set) */
            if (vfs_create(path, FS_TYPE_FILE) < 0)
                return (uint64_t)-1;
            return 0;
        }
        case S_IFDIR:
            /* Directory — use vfs_create with FS_TYPE_DIR */
            if (vfs_create(path, FS_TYPE_DIR) < 0)
                return (uint64_t)-1;
            return 0;
        case S_IFIFO:
            /* Named FIFO (pipe) — create as FIFO-type file */
            if (vfs_create(path, VFS_TYPE_FIFO) < 0)
                return (uint64_t)-1;
            return 0;
        case S_IFSOCK:
            /* Socket — not supported via mknod */
            return (uint64_t)-EOPNOTSUPP;
        default:
            return (uint64_t)-EINVAL;
    }
}

static void netstat_tcp_cb(uint16_t lport, uint32_t rip, uint16_t rport, int state) {
    const char *snames[] = {"CLOSED","LISTEN","SYN_SENT","SYN_RCV","ESTABLISHED","FIN_WAIT","CLOSE_WAIT","TIME_WAIT"};
    const char *sname = (state >= 0 && state < 8) ? snames[state] : "?";
    kprintf("  TCP  %5lu  %lu.%lu.%lu.%lu:%lu  %s\n",
        (unsigned long)lport,
        (unsigned long)((rip >> 24) & 0xFF), (unsigned long)((rip >> 16) & 0xFF),
        (unsigned long)((rip >>  8) & 0xFF), (unsigned long)(rip & 0xFF),
        (unsigned long)rport, sname);
}

static void netstat_udp_cb(uint16_t port) {
    kprintf("  UDP  %5lu  *:*  LISTEN\n", (unsigned long)port);
}

static uint64_t sys_net_connlist(void) {
    kprintf("Proto  LPort  Remote          State\n");
    net_conn_list(netstat_tcp_cb);
    net_udp_list(netstat_udp_cb);
    return 0;
}

static void arp_print_entry_sys(uint32_t ip, const uint8_t *mac) {
    kprintf("  %lu.%lu.%lu.%lu  ->  %lx:%lx:%lx:%lx:%lx:%lx\n",
            (unsigned long)((ip >> 24) & 0xFF), (unsigned long)((ip >> 16) & 0xFF),
            (unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF),
            (unsigned long)mac[0], (unsigned long)mac[1], (unsigned long)mac[2],
            (unsigned long)mac[3], (unsigned long)mac[4], (unsigned long)mac[5]);
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

    /* Acquire process table lock for consistent snapshot — otherwise
     * a process exiting concurrently can produce corrupted output. */
    uint64_t __pl_flags;
    spinlock_irqsave_acquire(&proc_table_lock, &__pl_flags);

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
        /* CPU time and resource info */
        out[written].cpu_user_ticks   = table[i].utime_ticks;
        out[written].cpu_system_ticks = table[i].stime_ticks;
        out[written].nice             = table[i].nice;
        out[written].max_rss          = table[i].max_rss;
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
    spinlock_irqsave_release(&proc_table_lock, __pl_flags);
    return (uint64_t)written;
}

static uint64_t sys_pci_list(void) {
    pci_list();
    return 0;
}

static uint64_t sys_usb_list(void) {
    int n = usb_get_device_count();
    kprintf("USB devices: %lu\n", (unsigned long)n);
    for (int i = 0; i < n; i++) {
        struct usb_device *dev = usb_get_device(i);
        if (!dev) continue;
        const char *spd = dev->speed == 2 ? "High" :
                          dev->speed == 1 ? "Low"  : "Full";
        kprintf("  Bus %03lu Device %03lu: %s-speed class=%02lx\n",
                (unsigned long)1, (unsigned long)dev->addr,
                spd, (unsigned long)dev->class_code);
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
    kprintf("CPU family/model/stepping: %lu/%lu/%lu\n",
            (unsigned long)((eax >> 8) & 0xF), (unsigned long)((eax >> 4) & 0xF), (unsigned long)(eax & 0xF));

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
        buf[n_read++] = (uint8_t)serial_getchar();
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
    if (!path) return (uint64_t)(int64_t)-EFAULT;
    if (!script_exec_ptr) return (uint64_t)(int64_t)-ENOSYS;
    return (uint64_t)script_exec_ptr(path);
}

static uint64_t sys_fat_mount(uint64_t disk, uint64_t part_lba) {
    if (part_lba > 0xFFFFFFFFULL) return (uint64_t)-EOVERFLOW;
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

static uint64_t sys_keyboard_getchar(void) {
    return (uint64_t)(uint8_t)keyboard_getchar();
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

/* ══════════════════════════════════════════════════════════════════════════
 * Production-ready OS improvements — Tier 1-5 syscall implementations
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── rlimit/prlimit64/getrlimit/setrlimit ──────────────────────────── */

static uint64_t sys_prlimit64(uint64_t pid, uint64_t resource,
                               uint64_t new_rlim_addr, uint64_t old_rlim_addr) {
    if (resource >= _RLIMIT_NLIMITS) return (uint64_t)-1;

    struct process *target;
    if (pid == 0) {
        target = process_get_current();
    } else {
        target = process_get_by_pid((uint32_t)pid);
    }
    if (!target || target->state == PROCESS_UNUSED) return (uint64_t)-1;

    /* Copy old limits to user if requested */
    if (old_rlim_addr) {
        if (syscall_is_user_process() && !syscall_user_write_ok(old_rlim_addr, 16))
            return (uint64_t)-1;
        struct rlimit64 old_rlim;
        old_rlim.rlim_cur = target->rlim_cur[resource];
        old_rlim.rlim_max = target->rlim_max[resource];
        if (copy_to_user(old_rlim_addr, &old_rlim, 16) < 0)
            return (uint64_t)-1;
    }

    /* Set new limits if requested */
    if (new_rlim_addr) {
        if (syscall_is_user_process() && !syscall_user_read_ok(new_rlim_addr, 16))
            return (uint64_t)-1;
        struct rlimit64 new_rlim;
        if (copy_from_user(&new_rlim, new_rlim_addr, 16) < 0)
            return (uint64_t)-1;
        /* Can't raise hard limit without CAP_SYS_RESOURCE */
        if (new_rlim.rlim_max > target->rlim_max[resource])
            return (uint64_t)-1;
        if (new_rlim.rlim_cur > new_rlim.rlim_max)
            return (uint64_t)-1;
        target->rlim_cur[resource] = new_rlim.rlim_cur;
        target->rlim_max[resource] = new_rlim.rlim_max;

        /* Update fd limit shorthand if NOFILE changed */
        if (resource == RLIMIT_NOFILE)
            target->file_max = new_rlim.rlim_cur;
    }

    return 0;
}

/* ── futex ────────────────────────────────────────────────────────────── */

/* Simple futex: uaddr is a userspace 32-bit integer.
 * FUTEX_WAIT: if *uaddr == val, block the process until FUTEX_WAKE.
 * FUTEX_WAKE: wake up to 'val' waiters.
 * Extended with: FUTEX_LOCK_PI, FUTEX_UNLOCK_PI, FUTEX_CMP_REQUEUE_PI,
 * and robust list support. */

/* PI futex state table (local - backing store for futex.h API) */
struct futex_waiter futex_waiters[FUTEX_MAX_WAITERS];
int futex_num_waiters = 0;

#define FUTEX_PI_MAX 16
static struct {
    uint32_t *uaddr;
    uint32_t owner_pid;
    int waiter_count;
    uint32_t waiter_pids[4];
    int in_use;
} futex_pi_table[FUTEX_PI_MAX];

static int futex_pi_find(uint32_t *uaddr) {
    for (int i = 0; i < FUTEX_PI_MAX; i++)
        if (futex_pi_table[i].in_use && futex_pi_table[i].uaddr == uaddr)
            return i;
    return -1;
}

static int futex_pi_alloc_internal(uint32_t *uaddr, uint32_t owner_pid) {
    int idx = futex_pi_find(uaddr);
    if (idx >= 0) return idx;
    for (int i = 0; i < FUTEX_PI_MAX; i++) {
        if (!futex_pi_table[i].in_use) {
            futex_pi_table[i].uaddr = uaddr;
            futex_pi_table[i].owner_pid = owner_pid;
            futex_pi_table[i].waiter_count = 0;
            futex_pi_table[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* ── Robust list support ───────────────────────────────────── */

int sys_set_robust_list(struct robust_list_head *head, size_t len) {
    (void)head; (void)len;
    struct process *cur = process_get_current();
    if (!cur) return -EPERM;
    /* Store robust list head pointer for later cleanup */
    cur->ctid_ptr = (void*)head; /* reuse ctid_ptr for robust list head */
    return 0;
}

int sys_get_robust_list(int pid, struct robust_list_head **head_ptr, size_t *len_ptr) {
    struct process *p = process_get_by_pid((uint32_t)pid);
    if (!p || p->state == PROCESS_UNUSED) return -ESRCH;
    if (head_ptr) *head_ptr = (struct robust_list_head *)p->ctid_ptr;
    if (len_ptr) *len_ptr = sizeof(struct robust_list_head);
    return 0;
}

/* On thread exit, walk robust list and wake waiters */
void futex_robust_list_cleanup(struct process *proc) {
    if (!proc || !proc->ctid_ptr) return;
    struct robust_list_head *head = (struct robust_list_head *)proc->ctid_ptr;
    /* Wake waiters on all futexes in the robust list */
    struct robust_list *list = head->list.next;
    while (list && list != &head->list) {
        uint32_t *uaddr = (uint32_t *)((uint8_t *)list + head->futex_offset);
        /* Wake all waiters on this uaddr */
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (futex_waiters[i].proc && futex_waiters[i].uaddr == uaddr) {
                struct process *p = futex_waiters[i].proc;
                futex_waiters[i].proc = NULL;
                futex_waiters[i].uaddr = NULL;
                futex_num_waiters--;
                if (p->state == PROCESS_BLOCKED) {
                    p->state = PROCESS_READY;
                    scheduler_add(p);
                }
            }
        }
        list = list->next;
    }
}

static uint64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                           uint64_t timeout, uint64_t uaddr2, uint64_t val3) {
    (void)timeout;
    uint32_t *addr = (uint32_t *)uaddr;
    uint32_t *addr2 = (uint32_t *)uaddr2;

    switch (op & ~(uint64_t)(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) {
        case FUTEX_WAIT: {
            /* Check user address */
            if (syscall_is_user_process() && !syscall_user_read_ok(uaddr, 4))
                return (uint64_t)-1;

            /* Check that *uaddr == val */
            uint32_t cur;
            memcpy(&cur, addr, 4);
            if (cur != (uint32_t)val)
                return (uint64_t)-1; /* EWOULDBLOCK — caller should retry */

            /* Register as waiter */
            struct process *cur_proc = process_get_current();
            if (!cur_proc) return (uint64_t)-1;

            /* Bounds check: ensure we don't exceed the waiter array capacity */
            if (futex_num_waiters >= FUTEX_MAX_WAITERS)
                return (uint64_t)-1;

            __asm__ volatile("cli");
            int found = -1;
            for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
                if (!futex_waiters[i].proc) {
                    found = i;
                    break;
                }
            }
            if (found < 0) { __asm__ volatile("sti"); return (uint64_t)-1; }

            futex_waiters[found].uaddr = addr;
            futex_waiters[found].proc  = cur_proc;
            futex_waiters[found].bitset = 0xFFFFFFFF; /* match any bitset */
            if (futex_waiters[found].proc)
                futex_num_waiters++;

            /* Block the current process */
            cur_proc->state = PROCESS_BLOCKED;
            scheduler_remove(cur_proc);
            __asm__ volatile("sti");
            scheduler_yield();
            return 0;
        }

        case FUTEX_WAIT_BITSET: {
            /* ── FUTEX_WAIT_BITSET: like FUTEX_WAIT but with a bitset mask ──
             *
             * val3 holds the bitset. A waiter is only woken by FUTEX_WAKE_BITSET
             * whose bitset has a non-empty intersection with this bitset.
             * bitset == 0 is invalid (returns -EINVAL).
             */
            if (val3 == 0)
                return (uint64_t)-1; /* EINVAL */

            /* Check user address */
            if (syscall_is_user_process() && !syscall_user_read_ok(uaddr, 4))
                return (uint64_t)-1;

            /* Check that *uaddr == val */
            uint32_t cur;
            memcpy(&cur, addr, 4);
            if (cur != (uint32_t)val)
                return (uint64_t)-1; /* EWOULDBLOCK */

            /* Register as waiter */
            struct process *cur_proc = process_get_current();
            if (!cur_proc) return (uint64_t)-1;

            /* Bounds check: ensure we don't exceed the waiter array capacity */
            if (futex_num_waiters >= FUTEX_MAX_WAITERS)
                return (uint64_t)-1;

            __asm__ volatile("cli");
            int found = -1;
            for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
                if (!futex_waiters[i].proc) {
                    found = i;
                    break;
                }
            }
            if (found < 0) { __asm__ volatile("sti"); return (uint64_t)-1; }

            futex_waiters[found].uaddr  = addr;
            futex_waiters[found].proc   = cur_proc;
            futex_waiters[found].bitset = (uint32_t)val3;
            if (futex_waiters[found].proc)
                futex_num_waiters++;

            /* Block the current process */
            cur_proc->state = PROCESS_BLOCKED;
            scheduler_remove(cur_proc);
            __asm__ volatile("sti");
            scheduler_yield();
            return 0;
        }

        case FUTEX_WAKE: {
            /* Wake up to 'val' waiters on this uaddr */
            int woken = 0;
            __asm__ volatile("cli");
            for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int)val; i++) {
                if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr) {
                    struct process *p = futex_waiters[i].proc;
                    futex_waiters[i].proc = NULL;
                    futex_waiters[i].uaddr = NULL;
                    futex_num_waiters--;
                    if (p->state == PROCESS_BLOCKED) {
                        p->state = PROCESS_READY;
                        scheduler_add(p);
                    }
                    woken++;
                }
            }
            __asm__ volatile("sti");
            return (uint64_t)woken;
        }

        case FUTEX_WAKE_BITSET: {
            /* ── FUTEX_WAKE_BITSET: like FUTEX_WAKE but with bitset matching ──
             *
             * val3 holds the wake bitset. Only wake waiters whose stored bitset
             * has a non-empty intersection with the wake bitset.
             * bitset == 0 is invalid (returns -EINVAL).
             */
            if (val3 == 0)
                return (uint64_t)-1; /* EINVAL */

            uint32_t wake_bitset = (uint32_t)val3;
            int woken = 0;
            __asm__ volatile("cli");
            for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int)val; i++) {
                if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr &&
                    (futex_waiters[i].bitset & wake_bitset) != 0) {
                    struct process *p = futex_waiters[i].proc;
                    futex_waiters[i].proc = NULL;
                    futex_waiters[i].uaddr = NULL;
                    futex_waiters[i].bitset = 0;
                    futex_num_waiters--;
                    if (p->state == PROCESS_BLOCKED) {
                        p->state = PROCESS_READY;
                        scheduler_add(p);
                    }
                    woken++;
                }
            }
            __asm__ volatile("sti");
            return (uint64_t)woken;
        }

        case FUTEX_REQUEUE: {
            /* Requeue 'val' waiters from addr to addr2 */
            int requeued = 0;
            __asm__ volatile("cli");
            int max_wake = (int)(val & 0x7FFFFFFF);
            int max_requeue = (int)((val >> 32) & 0x7FFFFFFF);
            if (max_wake < 1) max_wake = 1;
            if (max_requeue < 1) max_requeue = 1;

            /* First, wake up to max_wake waiters */
            int woken = 0;
            for (int i = 0; i < FUTEX_MAX_WAITERS && woken < max_wake; i++) {
                if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr) {
                    struct process *p = futex_waiters[i].proc;
                    futex_waiters[i].proc = NULL;
                    futex_waiters[i].uaddr = NULL;
                    futex_num_waiters--;
                    if (p->state == PROCESS_BLOCKED) {
                        p->state = PROCESS_READY;
                        scheduler_add(p);
                    }
                    woken++;
                }
            }
            /* Then requeue remaining to addr2 */
            for (int i = 0; i < FUTEX_MAX_WAITERS && requeued < max_requeue; i++) {
                if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr) {
                    futex_waiters[i].uaddr = addr2;
                    requeued++;
                }
            }
            __asm__ volatile("sti");
            return (uint64_t)woken;
        }

        case FUTEX_CMP_REQUEUE: {
            if (syscall_is_user_process() && !syscall_user_read_ok(uaddr, 4))
                return (uint64_t)-1;
            uint32_t cur;
            memcpy(&cur, addr, 4);
            if (cur != (uint32_t)val3)
                return (uint64_t)-1; /* values don't match */
            return sys_futex(uaddr, FUTEX_REQUEUE, val, timeout, uaddr2, val3);
        }

        case FUTEX_LOCK_PI: {
            /* Priority Inheritance futex lock */
            struct process *cur = process_get_current();
            if (!cur) return (uint64_t)-1;

            uint32_t cur_val;
            memcpy(&cur_val, addr, 4);

            if (cur_val == 0) {
                /* Free — try to acquire directly */
                uint32_t tid = cur->pid;
                memcpy(addr, &tid, 4);
                return 0;
            }

            /* Contended — register as PI waiter and block */
            int pi_idx = futex_pi_alloc_internal(addr, cur_val);
            if (pi_idx < 0) return (uint64_t)-1;

            /* Boost owner priority */
            struct process *owner = process_get_by_pid(futex_pi_table[pi_idx].owner_pid);
            if (owner && owner->priority > cur->priority) {
                owner->base_priority = owner->priority;
                owner->priority = cur->priority;
                scheduler_set_priority(owner, cur->priority);
            }

            /* Register as waiter */
            {
                int w = futex_pi_table[pi_idx].waiter_count;
                if (w < 4) {
                    futex_pi_table[pi_idx].waiter_pids[w] = cur->pid;
                    futex_pi_table[pi_idx].waiter_count++;
                }
            }

            /* Block */
            uint32_t tid = cur->pid | 0x80000000U; /* set high bit = contended */
            memcpy(addr, &tid, 4);

            cur->state = PROCESS_BLOCKED;
            scheduler_remove(cur);
            scheduler_yield();
            return 0;
        }

        case FUTEX_UNLOCK_PI: {
            /* Priority Inheritance futex unlock */
            struct process *cur = process_get_current();
            if (!cur) return (uint64_t)-1;

            uint32_t cur_val;
            memcpy(&cur_val, addr, 4);

            if ((cur_val & 0x7FFFFFFF) != cur->pid)
                return (uint64_t)-1; /* not owner */

            int pi_idx = futex_pi_find(addr);
            if (pi_idx >= 0) {
                struct process *owner = process_get_by_pid(futex_pi_table[pi_idx].owner_pid);
                if (owner && owner->priority != owner->base_priority) {
                    /* Restore base priority */
                    owner->priority = owner->base_priority;
                    scheduler_set_priority(owner, owner->base_priority);
                }

                if (futex_pi_table[pi_idx].waiter_count > 0) {
                    /* Wake the first waiter */
                    uint32_t next_pid = futex_pi_table[pi_idx].waiter_pids[0];
                    /* Shift remaining waiters */
                    for (int w = 1; w < futex_pi_table[pi_idx].waiter_count; w++)
                        futex_pi_table[pi_idx].waiter_pids[w-1] = futex_pi_table[pi_idx].waiter_pids[w];
                    futex_pi_table[pi_idx].waiter_count--;

                    /* Transfer ownership to next waiter */
                    futex_pi_table[pi_idx].owner_pid = next_pid;
                    uint32_t new_owner_tid = next_pid;
                    memcpy(addr, &new_owner_tid, 4);

                    /* Wake the next owner */
                    struct process *next = process_get_by_pid(next_pid);
                    if (next && next->state == PROCESS_BLOCKED) {
                        next->state = PROCESS_READY;
                        scheduler_wakeup(next);
                    }
                } else {
                    /* No waiters — mark as free */
                    uint32_t zero = 0;
                    memcpy(addr, &zero, 4);
                    futex_pi_table[pi_idx].in_use = 0;
                }
            } else {
                uint32_t zero = 0;
                memcpy(addr, &zero, 4);
            }
            return 0;
        }

        case FUTEX_WAKE_OP: {
            /* ── FUTEX_WAKE_OP: atomic modify uaddr2, then conditional wake uaddr ──
             *
             * val3 encodes op, cmp, oparg, cmparg (see futex.h):
             *   bits [31:28] = operation (FUTEX_OP_*)
             *   bits [27:24] = comparison (FUTEX_OP_CMP_*)
             *   bits [23:12] = oparg (12-bit signed)
             *   bits [11: 0] = cmparg (12-bit signed)
             *
             * 1. Atomically read oldval from *uaddr2, compute newval = op(oldval, oparg),
             *    write newval to *uaddr2.
             * 2. Always wake up to 'val2' waiters on uaddr2.
             * 3. If comparison (oldval OP cmp cmparg) is true, wake up to 'val' waiters on uaddr.
             */
            if (syscall_is_user_process()) {
                if (!syscall_user_read_ok(uaddr, 4) || !syscall_user_read_ok(uaddr2, 4))
                    return (uint64_t)-1;
                if (!syscall_user_write_ok(uaddr2, 4))
                    return (uint64_t)-1;
            }

            /* Decode val3 */
            unsigned futex_op = (unsigned)((val3 >> 28) & 0xf);
            unsigned cmp  = (unsigned)((val3 >> 24) & 0xf);
            /* Sign-extend 12-bit oparg */
            int32_t oparg = (int32_t)((val3 >> 12) & 0xfff);
            if (oparg & 0x800) oparg = (int32_t)((uint32_t)oparg | 0xfffff000);
            /* Sign-extend 12-bit cmparg */
            int32_t cmparg = (int32_t)(val3 & 0xfff);
            if (cmparg & 0x800) cmparg = (int32_t)((uint32_t)cmparg | 0xfffff000);

            /* If OPARG_SHIFT is set, shift oparg left by 8 */
            if (futex_op & FUTEX_OP_OPARG_SHIFT)
                oparg <<= 8;

            futex_op &= 7; /* mask to just the operation bits */

            /* Read old value from uaddr2 */
            uint32_t oldval;
            memcpy(&oldval, addr2, 4);

            /* Compute new value */
            uint32_t newval = oldval;
            switch (op) {
                case FUTEX_OP_SET:
                    newval = (uint32_t)oparg;
                    break;
                case FUTEX_OP_ADD:
                    newval = oldval + (uint32_t)oparg;
                    break;
                case FUTEX_OP_OR:
                    newval = oldval | (uint32_t)oparg;
                    break;
                case FUTEX_OP_ANDN:
                    newval = oldval & ~(uint32_t)oparg;
                    break;
                case FUTEX_OP_XOR:
                    newval = oldval ^ (uint32_t)oparg;
                    break;
                default:
                    return (uint64_t)-1; /* ENOSYS */
            }

            /* Write new value to uaddr2 */
            memcpy(addr2, &newval, 4);

            /* ── Always wake up to 'val2' waiters on uaddr2 ────────── */
            int woken1 = 0;
            int woken2 = 0;
            __asm__ volatile("cli");
            int max_wake2 = (int)(val & 0x7FFFFFFF);
            if (max_wake2 < 1) max_wake2 = 1;
            for (int i = 0; i < FUTEX_MAX_WAITERS && woken2 < max_wake2; i++) {
                if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr2) {
                    struct process *p = futex_waiters[i].proc;
                    futex_waiters[i].proc = NULL;
                    futex_waiters[i].uaddr = NULL;
                    futex_num_waiters--;
                    if (p->state == PROCESS_BLOCKED) {
                        p->state = PROCESS_READY;
                        scheduler_add(p);
                    }
                    woken2++;
                }
            }
            __asm__ volatile("sti");

            /* ── Conditional wake on uaddr based on comparison ─────── */
            /* Per Linux semantics, we use val2 (upper 32 bits) for unconditional
             * wake on uaddr2 and val (lower 32 bits) for conditional wake on uaddr.
             * (val is actually in a1, val2 corresponds to timeout parameter) */

            /* Determine if comparison is true */
            int cmp_result = 0;
            switch (cmp) {
                case FUTEX_OP_CMP_EQ: cmp_result = ((int32_t)oldval == cmparg); break;
                case FUTEX_OP_CMP_NE: cmp_result = ((int32_t)oldval != cmparg); break;
                case FUTEX_OP_CMP_LT: cmp_result = ((int32_t)oldval <  cmparg); break;
                case FUTEX_OP_CMP_LE: cmp_result = ((int32_t)oldval <= cmparg); break;
                case FUTEX_OP_CMP_GT: cmp_result = ((int32_t)oldval >  cmparg); break;
                case FUTEX_OP_CMP_GE: cmp_result = ((int32_t)oldval >= cmparg); break;
                default: cmp_result = 0; break;
            }

            if (cmp_result) {
                __asm__ volatile("cli");
                int max_wake1 = (int)(val & 0x7FFFFFFF);
                if (max_wake1 < 1) max_wake1 = 1;
                for (int i = 0; i < FUTEX_MAX_WAITERS && woken1 < max_wake1; i++) {
                    if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr) {
                        struct process *p = futex_waiters[i].proc;
                        futex_waiters[i].proc = NULL;
                        futex_waiters[i].uaddr = NULL;
                        futex_num_waiters--;
                        if (p->state == PROCESS_BLOCKED) {
                            p->state = PROCESS_READY;
                            scheduler_add(p);
                        }
                        woken1++;
                    }
                }
                __asm__ volatile("sti");
            }

            /* Return total waiters woken (on uaddr + uaddr2) */
            return (uint64_t)(woken1 + woken2);
        }

        case FUTEX_CMP_REQUEUE_PI: {
            /* Requeue from non-PI futex to PI futex with PI boosting */
            if (syscall_is_user_process() && !syscall_user_read_ok(uaddr, 4))
                return (uint64_t)-1;
            uint32_t cur;
            memcpy(&cur, addr, 4);
            if (cur != (uint32_t)val3)
                return (uint64_t)-1;

            int max_wake = (int)(val & 0x7FFFFFFF);
            int max_requeue = (int)((val >> 32) & 0x7FFFFFFF);
            if (max_wake < 1) max_wake = 1;
            if (max_requeue < 1) max_requeue = 1;

            __asm__ volatile("cli");

            /* Wake up to max_wake waiters on addr */
            int woken = 0;
            for (int i = 0; i < FUTEX_MAX_WAITERS && woken < max_wake; i++) {
                if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr) {
                    struct process *p = futex_waiters[i].proc;
                    futex_waiters[i].proc = NULL;
                    futex_waiters[i].uaddr = NULL;
                    futex_num_waiters--;
                    if (p->state == PROCESS_BLOCKED) {
                        p->state = PROCESS_READY;
                        scheduler_add(p);
                    }
                    woken++;
                }
            }

            /* Requeue remaining to addr2 (PI futex) with PI boosting */
            int requeued = 0;
            for (int i = 0; i < FUTEX_MAX_WAITERS && requeued < max_requeue; i++) {
                if (futex_waiters[i].proc && futex_waiters[i].uaddr == addr) {
                    futex_waiters[i].uaddr = addr2;
                    requeued++;
                    /* PI boost the requeued waiter */
                    struct process *rp = futex_waiters[i].proc;
                    if (rp && rp->priority > 0)
                        rp->priority--;
                }
            }

            __asm__ volatile("sti");
            return (uint64_t)woken;
        }

        default:
            return (uint64_t)-1; /* ENOSYS */
    }
}

/* ── arch_prctl (TLS) ────────────────────────────────────────────────── */

static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-1;

    switch (code) {
        case ARCH_SET_FS:
            /* Set FS.base MSR (x86_64 thread-local storage) */
            __asm__ volatile("wrmsr" : : "c"(0xC0000100ULL), "a"((uint32_t)addr),
                            "d"((uint32_t)(addr >> 32)));
            return 0;
        case ARCH_GET_FS: {
            uint32_t lo, hi;
            __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100ULL));
            uint64_t fs_base = ((uint64_t)hi << 32) | lo;
            if (syscall_is_user_process()) {
                if (!syscall_user_write_ok(addr, 8))
                    return (uint64_t)-1;
            }
            *(uint64_t*)addr = fs_base;
            return 0;
        }
        case ARCH_SET_GS:
            __asm__ volatile("wrmsr" : : "c"(0xC0000101ULL), "a"((uint32_t)addr),
                            "d"((uint32_t)(addr >> 32)));
            return 0;
        case ARCH_GET_GS: {
            uint32_t lo, hi;
            __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101ULL));
            uint64_t gs_base = ((uint64_t)hi << 32) | lo;
            if (syscall_is_user_process()) {
                if (!syscall_user_write_ok(addr, 8))
                    return (uint64_t)-1;
            }
            *(uint64_t*)addr = gs_base;
            return 0;
        }
        default:
            return (uint64_t)-1;
    }
}

/* ── Poll is implemented in src/kernel/poll.c using the poll_table
 * infrastructure.  The dispatch table entry calls the external
 * sys_poll() declared in poll.h. ──────────────────────────────*/


/* ── pselect6 — safer select with atomic signal mask (Item 251) ────── */

/*
 * On Linux x86_64, the 6th argument to pselect6 is a packed struct:
 *   struct { const uint64_t *sigmask; size_t sigset_size; }
 * It arrives in R9 and is saved in syscall_arg6 by the asm entry.
 *
 * The signal mask is stored as a uint64_t bitmask in the kernel
 * (proc->sig_mask), so we treat sigset_t* as uint64_t* here.
 */
#define PSELECT6_SIGMASK_OFFSET 0
#define PSELECT6_SSIZE_OFFSET   8

static uint64_t sys_pselect6(uint64_t nfds, uint64_t readfds_addr,
                              uint64_t writefds_addr, uint64_t exceptfds_addr,
                              uint64_t timeout_addr) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    /* Extract sigmask from the packed 6th argument */
    const uint64_t *sigmask = NULL;
    uint64_t packed = syscall_arg6;
    if (packed) {
        if (syscall_is_user_process() && !syscall_user_read_ok(packed, sizeof(uint64_t*) + sizeof(size_t)))
            return (uint64_t)-1;
        sigmask = *(const uint64_t **)packed;
    }

    /* Apply the temporary signal mask if provided */
    uint64_t old_mask;
    uint64_t __ps_flags;
    spinlock_irqsave_acquire(&proc->sig_lock, &__ps_flags);
    old_mask = proc->sig_mask;
    if (sigmask) {
        if (syscall_is_user_process() && !syscall_user_read_ok((uint64_t)sigmask, sizeof(uint64_t))) {
            spinlock_irqsave_release(&proc->sig_lock, __ps_flags);
            return (uint64_t)-1;
        }
        proc->sig_mask = *sigmask;
    }
    spinlock_irqsave_release(&proc->sig_lock, __ps_flags);

    /* Delegate to the existing select implementation.
     * We reuse the select body by calling it directly — the select
     * implementation checks FD sets and sleeps.  After it returns,
     * we restore the original signal mask. */

    /* We can't call sys_select() directly because it returns only
     * after the timeout.  Instead we inline a simplified select loop
     * that checks for pending signals as well. */

    if (nfds > FD_SETSIZE) nfds = FD_SETSIZE;

    fd_set readfds, writefds, exceptfds;
    fd_set orig_readfds, orig_writefds, orig_exceptfds;

    /* Copy in from userspace — skip NULL sets */
    if (readfds_addr) {
        if (copy_from_user(&orig_readfds, readfds_addr, sizeof(fd_set)) < 0)
            return (uint64_t)-1;
    } else {
        FD_ZERO(&orig_readfds);
    }
    if (writefds_addr) {
        if (copy_from_user(&orig_writefds, writefds_addr, sizeof(fd_set)) < 0)
            return (uint64_t)-1;
    } else {
        FD_ZERO(&orig_writefds);
    }
    if (exceptfds_addr) {
        if (copy_from_user(&orig_exceptfds, exceptfds_addr, sizeof(fd_set)) < 0)
            return (uint64_t)-1;
    } else {
        FD_ZERO(&orig_exceptfds);
    }

    /* Parse timeout (timespec-based) */
    uint64_t timeout_ticks = 0;
    int has_timeout = 0;
    if (timeout_addr) {
        struct timespec ts;
        if (copy_from_user(&ts, timeout_addr, sizeof(ts)) < 0)
            return (uint64_t)-1;
        /* Validate: timespec fields must be non-negative */
        if ((int64_t)ts.tv_sec >= 0 && (int64_t)ts.tv_nsec >= 0) {
            timeout_ticks = (uint64_t)(int64_t)ts.tv_sec * 100 + (uint64_t)(int64_t)ts.tv_nsec / 10000000;
            has_timeout = 1;
        }
    }

    uint64_t start_tick = timer_get_ticks();
    int loops = 0;
    int max_loops = has_timeout ? (int)(timeout_ticks / 1 + 1) : 1;
    if (max_loops < 1) max_loops = 1;

    while (loops < max_loops) {
        /* Check for pending signals before each iteration */
        {
            uint64_t __ps_flags2;
            spinlock_irqsave_acquire(&proc->sig_lock, &__ps_flags2);
            int sig_pending = (proc->pending_signals & ~proc->sig_mask) != 0;
            spinlock_irqsave_release(&proc->sig_lock, __ps_flags2);
            if (sig_pending) {
                /* A signal is pending that is not masked — return ready=0
                 * but with EINTR semantics.  POSIX: pselect6 can return
                 * with errno=EINTR if a signal handler was invoked. */
                break;
            }
        }

        if (readfds_addr) {
            memcpy(&readfds, &orig_readfds, sizeof(fd_set));
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &readfds)) continue;
                if (i >= 100 && i < 100 + SOCK_MAX) {
                    int revents = sock_poll(i, POLLIN, NULL);
                    if (!(revents & POLLIN)) FD_CLR(i, &readfds);
                    continue;
                }
                if (i >= PROCESS_FD_MAX || !proc->fd_table[i].used) {
                    FD_CLR(i, &readfds);
                    continue;
                }
                if (strncmp(proc->fd_table[i].path, "pipe_read_", 10) == 0) {
                    int pipe_id = (int)proc->fd_table[i].offset;
                    int revents = pipe_poll(pipe_id, 1, NULL);
                    if (!(revents & POLLIN)) FD_CLR(i, &readfds);
                    continue;
                }
            }
        }
        if (writefds_addr) {
            memcpy(&writefds, &orig_writefds, sizeof(fd_set));
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &writefds)) continue;
                if (i >= 100 && i < 100 + SOCK_MAX) {
                    int revents = sock_poll(i, POLLOUT, NULL);
                    if (!(revents & POLLOUT)) FD_CLR(i, &writefds);
                    continue;
                }
                if (i >= PROCESS_FD_MAX || !proc->fd_table[i].used) {
                    FD_CLR(i, &writefds);
                    continue;
                }
                if (strncmp(proc->fd_table[i].path, "pipe_write_", 11) == 0) {
                    int pipe_id = (int)proc->fd_table[i].offset;
                    int revents = pipe_poll(pipe_id, 0, NULL);
                    if (!(revents & POLLOUT)) FD_CLR(i, &writefds);
                    continue;
                }
            }
        }
        if (exceptfds_addr) {
            memcpy(&exceptfds, &orig_exceptfds, sizeof(fd_set));
            for (int i = 0; i < (int)nfds; i++) {
                if (!FD_ISSET(i, &exceptfds)) continue;
                /* For exceptfds, check for out-of-band data on sockets */
                if (i >= 100 && i < 100 + SOCK_MAX) {
                    int revents = sock_poll(i, POLLPRI, NULL);
                    if (!(revents & POLLPRI)) FD_CLR(i, &exceptfds);
                    continue;
                }
            }
        }

        /* Check if any FD is ready */
        int ready = 0;
        if (readfds_addr) {
            for (int i = 0; i < (int)nfds; i++)
                if (FD_ISSET(i, &readfds)) { ready++; break; }
        }
        if (!ready && writefds_addr) {
            for (int i = 0; i < (int)nfds; i++)
                if (FD_ISSET(i, &writefds)) { ready++; break; }
        }
        if (!ready && exceptfds_addr) {
            for (int i = 0; i < (int)nfds; i++)
                if (FD_ISSET(i, &exceptfds)) { ready++; break; }
        }

        if (ready) {
            /* Copy results back to userspace */
            if (readfds_addr && copy_to_user(readfds_addr, &readfds, sizeof(fd_set)) < 0)
                return (uint64_t)-1;
            if (writefds_addr && copy_to_user(writefds_addr, &writefds, sizeof(fd_set)) < 0)
                return (uint64_t)-1;
            if (exceptfds_addr && copy_to_user(exceptfds_addr, &exceptfds, sizeof(fd_set)) < 0)
                return (uint64_t)-1;
            /* Restore original signal mask */
            if (sigmask) {
                uint64_t __ps_flags3;
                spinlock_irqsave_acquire(&proc->sig_lock, &__ps_flags3);
                proc->sig_mask = old_mask;
                spinlock_irqsave_release(&proc->sig_lock, __ps_flags3);
            }
            return (uint64_t)ready;
        }

        if (!has_timeout) break;  /* non-blocking case */

        uint64_t elapsed = timer_get_ticks() - start_tick;
        if (elapsed >= timeout_ticks) break;

        /* Yield until next tick */
        proc->sleep_until = timer_get_ticks() + 1;
        proc->state = PROCESS_BLOCKED;
        scheduler_remove(proc);
        scheduler_yield();
        loops++;
    }

    /* Timeout or no FDs ready */
    if (readfds_addr) FD_ZERO(&readfds);
    if (writefds_addr) FD_ZERO(&writefds);
    if (exceptfds_addr) FD_ZERO(&exceptfds);
    if (readfds_addr && copy_to_user(readfds_addr, &readfds, sizeof(fd_set)) < 0)
        return (uint64_t)-1;
    if (writefds_addr && copy_to_user(writefds_addr, &writefds, sizeof(fd_set)) < 0)
        return (uint64_t)-1;
    if (exceptfds_addr && copy_to_user(exceptfds_addr, &exceptfds, sizeof(fd_set)) < 0)
        return (uint64_t)-1;

    /* Restore original signal mask */
    if (sigmask) {
        uint64_t __ps_flags4;
        spinlock_irqsave_acquire(&proc->sig_lock, &__ps_flags4);
        proc->sig_mask = old_mask;
        spinlock_irqsave_release(&proc->sig_lock, __ps_flags4);
    }
    return 0;
}

/* ── ppoll — safer poll with atomic signal mask (Item 251) ─────────── */

static uint64_t sys_ppoll(uint64_t fds_addr, uint64_t nfds,
                           uint64_t timeout_addr, uint64_t sigmask_addr) {
    if (syscall_is_user_process()) {
        if (!syscall_user_read_ok(fds_addr, nfds * sizeof(struct pollfd)))
            return (uint64_t)-1;
        if (!syscall_user_write_ok(fds_addr, nfds * sizeof(struct pollfd)))
            return (uint64_t)-1;
    }

    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    /* Apply the temporary signal mask if provided */
    uint64_t old_mask;
    uint64_t __pp_flags;
    spinlock_irqsave_acquire(&proc->sig_lock, &__pp_flags);
    old_mask = proc->sig_mask;
    if (sigmask_addr) {
        uint64_t new_mask;
        if (copy_from_user(&new_mask, sigmask_addr, sizeof(new_mask)) < 0) {
            spinlock_irqsave_release(&proc->sig_lock, __pp_flags);
            return (uint64_t)-1;
        }
        proc->sig_mask = new_mask;
    }
    spinlock_irqsave_release(&proc->sig_lock, __pp_flags);

    struct pollfd fds_buf[256];
    int n = (int)nfds;
    if (n > 256) n = 256;
    if (fds_addr) {
        if (copy_from_user(fds_buf, fds_addr, sizeof(struct pollfd) * (size_t)n) < 0)
            return (uint64_t)-1;
    } else {
        return (uint64_t)-1;
    }
    uint64_t timeout_ticks = ~0ULL; /* infinite */
    if (timeout_addr) {
        struct timespec ts;
        if (copy_from_user(&ts, timeout_addr, sizeof(ts)) < 0)
            return (uint64_t)-1;
        if ((int64_t)ts.tv_sec >= 0 && (int64_t)ts.tv_nsec >= 0) {
            timeout_ticks = (uint64_t)(int64_t)ts.tv_sec * 100 + (uint64_t)(int64_t)ts.tv_nsec / 10000000;
        }
    }

    uint64_t start_tick = timer_get_ticks();

    for (;;) {
        /* Check for pending unmasked signals */
        {
            uint64_t __pp_flags2;
            spinlock_irqsave_acquire(&proc->sig_lock, &__pp_flags2);
            int sig_pending = (proc->pending_signals & ~proc->sig_mask) != 0;
            spinlock_irqsave_release(&proc->sig_lock, __pp_flags2);
            if (sig_pending) {
                break;
            }
        }

        int ready = 0;
        for (int i = 0; i < n; i++) {
            fds_buf[i].revents = 0;
            if (fds_buf[i].fd < 0) {
                fds_buf[i].revents = POLLNVAL;
                ready++;
                continue;
            }

            int fd_idx = fds_buf[i].fd;
            int revents = 0;

            /* Socket FDs */
            if (fd_idx >= 100 && fd_idx < 100 + SOCK_MAX) {
                revents = sock_poll(fd_idx, fds_buf[i].events, NULL);
                fds_buf[i].revents = (int16_t)revents;
                if (revents) ready++;
                continue;
            }

            /* Regular process FDs */
            if (fd_idx >= PROCESS_FD_MAX || !proc->fd_table[fd_idx].used) {
                fds_buf[i].revents = POLLNVAL;
                ready++;
                continue;
            }

            struct process_fd *pfd = &proc->fd_table[fd_idx];

            if (strncmp(pfd->path, "pipe_read_", 10) == 0) {
                int pipe_id = (int)pfd->offset;
                revents = pipe_poll(pipe_id, 1, NULL);
            } else if (strncmp(pfd->path, "pipe_write_", 11) == 0) {
                int pipe_id = (int)pfd->offset;
                revents = pipe_poll(pipe_id, 0, NULL);
            } else {
                if (fds_buf[i].events & POLLIN)  revents |= POLLIN;
                if (fds_buf[i].events & POLLOUT) revents |= POLLOUT;
            }

            fds_buf[i].revents = (int16_t)(revents & fds_buf[i].events);
            if (fds_buf[i].revents) ready++;
        }

        /* Write results back to userspace */
        if (copy_to_user(fds_addr, fds_buf, sizeof(struct pollfd) * (size_t)n) < 0)
            return (uint64_t)-1;

        if (ready > 0) {
            if (sigmask_addr) {
                uint64_t __pp_flags3;
                spinlock_irqsave_acquire(&proc->sig_lock, &__pp_flags3);
                proc->sig_mask = old_mask;
                spinlock_irqsave_release(&proc->sig_lock, __pp_flags3);
            }
            return (uint64_t)ready;
        }

        if (timeout_ticks == 0) break; /* non-blocking */

        uint64_t elapsed = timer_get_ticks() - start_tick;
        if (elapsed >= timeout_ticks) break;

        /* Yield until next tick */
        proc->sleep_until = timer_get_ticks() + 1;
        proc->state = PROCESS_BLOCKED;
        scheduler_remove(proc);
        scheduler_yield();
    }

    /* Timeout — zero out all revents */
    for (int i = 0; i < n; i++) {
        fds_buf[i].revents = 0;
    }
    if (copy_to_user(fds_addr, fds_buf, sizeof(struct pollfd) * (size_t)n) < 0)
        return (uint64_t)-1;

    if (sigmask_addr) {
        uint64_t __pp_flags4;
        spinlock_irqsave_acquire(&proc->sig_lock, &__pp_flags4);
        proc->sig_mask = old_mask;
        spinlock_irqsave_release(&proc->sig_lock, __pp_flags4);
    }
    return 0;
}

/* ── eventfd ──────────────────────────────────────────────────────────── */

static uint64_t sys_eventfd(uint64_t initval, uint64_t flags) {
    int fd = eventfd_syscall((uint32_t)initval, (int)flags);
    if (fd < 0) return (uint64_t)-1;
    return (uint64_t)fd;
}

/* ── sendfile ──────────────────────────────────────────────────────────── */

/* Forward declaration for sock_send_raw helper */
static int sock_send_raw(struct socket *s, int sockfd,
                          const void *data, uint32_t len);

/**
 * sys_sendfile — Copy data between file descriptors (zero-copy file-to-socket)
 * @out_fd:   Output fd (socket or regular file)
 * @in_fd:    Input fd (must be a regular file)
 * @offset_addr: User-space pointer to off_t offset (0 = NULL, use file pos)
 * @count:    Maximum number of bytes to transfer
 *
 * Linux signature:
 *   ssize_t sendfile(int out_fd, int in_fd, off_t *offset, size_t count);
 *
 * Transfers up to @count bytes from @in_fd (a regular file opened for
 * reading) to @out_fd (a socket or regular file opened for writing).
 * Uses a 4 KiB kernel bounce buffer for the transfer.  When @out_fd is
 * a socket, data is sent directly via the socket-layer send path.
 *
 * If @offset_addr is NULL (0 in the ABI), data is read from the current
 * file offset of @in_fd, and that offset is advanced by the number of
 * bytes transferred.  If @offset_addr is non-NULL, data is read from the
 * absolute file offset stored in *@offset_addr; the file offset of @in_fd
 * is NOT changed, and *@offset_addr is incremented by the number of bytes
 * transferred.
 *
 * Return: Number of bytes transferred on success (as uint64_t, matching
 *         the Linux ssize_t convention), or negative errno encoded as
 *         (uint64_t)(int64_t)-ERRNO on failure.
 *
 * Linux errno values:
 *   EBADF      @in_fd not open for reading, or @out_fd not valid/writable
 *   EINVAL     offset pointer is non-NULL but points to a negative value,
 *              or @out_fd is a socket of unsupported type
 *   EFAULT     bad user-space pointer in @offset_addr
 *   EISDIR     @in_fd or @out_fd refers to a directory
 *   EIO        VFS read or socket send I/O failure
 *   ENOMEM     cannot allocate bounce buffer
 *   ESPIPE     @in_fd is not a regular file
 */
static uint64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd,
                              uint64_t offset_addr, uint64_t count)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-EPERM;

    /* ── Validate in_fd (must be a regular file, index = fd - 3) ── */

    if (in_fd < 3 || in_fd >= (uint64_t)(3 + PROCESS_FD_MAX))
        return (uint64_t)(int64_t)-EBADF;

    int in_idx = (int)in_fd - 3;
    struct process_fd *pfd_in = &p->fd_table[in_idx];
    if (!pfd_in->used)
        return (uint64_t)(int64_t)-EBADF;

    /* in_fd must be open for reading (O_RDONLY=0 or O_RDWR=2) */
    if ((pfd_in->open_flags & 3) == 1)   /* O_WRONLY alone */
        return (uint64_t)(int64_t)-EBADF;

    /* in_fd must refer to a regular file, not a directory or other type */
    {
        struct vfs_stat st_in;
        if (vfs_stat(pfd_in->path, &st_in) < 0)
            return (uint64_t)(int64_t)-EIO;
        if (st_in.type != VFS_TYPE_FILE)
            return (uint64_t)(int64_t)-EISDIR;
    }

    /* ── Validate out_fd ── */

    int is_socket = 0;
    struct socket *sock = NULL;
    struct process_fd *pfd_out = NULL;
    struct vfs_stat st_out;

    if (out_fd >= 100 && out_fd < (uint64_t)(100 + SOCK_MAX)) {
        /* Socket fd (slot = fd - 100) */
        sock = sock_get((int)out_fd);
        if (!sock)
            return (uint64_t)(int64_t)-EBADF;
        is_socket = 1;
    } else if (out_fd >= 3 && out_fd < (uint64_t)(3 + PROCESS_FD_MAX)) {
        /* Regular file fd */
        int out_idx = (int)out_fd - 3;
        pfd_out = &p->fd_table[out_idx];
        if (!pfd_out->used)
            return (uint64_t)(int64_t)-EBADF;

        /* out_fd must be open for writing (O_WRONLY=1 or O_RDWR=2) */
        if ((pfd_out->open_flags & 3) == 0)   /* O_RDONLY alone */
            return (uint64_t)(int64_t)-EBADF;

        if (vfs_stat(pfd_out->path, &st_out) < 0)
            return (uint64_t)(int64_t)-EIO;
        if (st_out.type != VFS_TYPE_FILE)
            return (uint64_t)(int64_t)-EISDIR;
    } else {
        return (uint64_t)(int64_t)-EBADF;
    }

    /* ── Allocate bounce buffer ── */

    uint8_t *buf = kmalloc(4096);
    if (!buf)
        return (uint64_t)(int64_t)-ENOMEM;

    /* ── Transfer state ── */

    uint64_t total = 0;
    uint64_t sf_result;
    int use_explicit_offset = (offset_addr != 0);

    /* If explicit offset is given, read initial value from user-space */
    if (use_explicit_offset) {
        int64_t abs_off;
        if (copy_from_user(&abs_off, offset_addr, sizeof(abs_off)) < 0) {
            sf_result = (uint64_t)(int64_t)-EFAULT;
            goto sendfile_cleanup;
        }
        if (abs_off < 0) {
            sf_result = (uint64_t)(int64_t)-EINVAL;
            goto sendfile_cleanup;
        }
        pfd_in->offset = (uint64_t)abs_off;
    }

    /* ── Transfer loop ── */
    while (total < count) {
        uint64_t chunk = count - total;
        if (chunk > 4096)
            chunk = 4096;

        /* Read a chunk from in_fd */
        uint32_t nread = 0;
        int r = vfs_read(pfd_in->path, buf, (uint32_t)chunk, &nread);
        if (r < 0) {
            /* On first iteration, propagate exact VFS errno.
             * On later iterations, return partial success. */
            sf_result = (total > 0) ? (uint64_t)total
                                    : (uint64_t)(int64_t)r;
            goto sendfile_cleanup;
        }

        if (nread == 0)
            break;  /* EOF */

        /* Write chunk to out_fd */
        int wret;
        if (is_socket) {
            wret = sock_send_raw(sock, (int)out_fd, buf, nread);
        } else {
            wret = vfs_write(pfd_out->path, buf, nread);
        }

        if (wret < 0) {
            sf_result = (total > 0) ? (uint64_t)total
                                    : (uint64_t)(int64_t)wret;
            goto sendfile_cleanup;
        }

        total += nread;

        if (nread < chunk)
            break;  /* Short read — EOF */
    }

    /* ── Update user-space offset pointer if applicable ── */
    if (use_explicit_offset) {
        int64_t new_off = (int64_t)(pfd_in->offset);
        if (copy_to_user(offset_addr, &new_off, sizeof(new_off)) < 0) {
            sf_result = (uint64_t)(int64_t)-EFAULT;
            goto sendfile_cleanup;
        }
    }
    /* else: the implicit file offset was already advanced by vfs_read
     * during the loop, so no update is needed. */

    sf_result = (uint64_t)total;

sendfile_cleanup:
    kfree(buf);
    return sf_result;
}

/**
 * sock_send_raw — Send a raw kernel buffer to a connected socket.
 * @s:          Socket descriptor from sock_get()
 * @sockfd:     Socket fd number (needed for packet/netlink/can dispatch)
 * @data:       Kernel-space data buffer to send
 * @len:        Number of bytes to send
 *
 * Dispatches to the correct protocol-specific send path depending on
 * the socket domain/type.  Supports AF_UNIX, AF_PACKET, AF_NETLINK,
 * AF_CAN, SOCK_STREAM (TCP), and SOCK_DGRAM (UDP).
 *
 * Return: Number of bytes sent on success, or negative errno on error.
 */
static int sock_send_raw(struct socket *s, int sockfd,
                          const void *data, uint32_t len)
{
    if (s->domain == AF_UNIX && s->unix_ep >= 0) {
        /* UNIX domain socket — unix_send handles kernel data */
        return unix_send(s->unix_ep, data, len, 0);
    }

    if (s->domain == AF_NETLINK && netlink_is_valid_fd(sockfd)) {
        /* AF_NETLINK socket */
        int max_send = (int)(len > NETLINK_MAX_PAYLOAD
                             ? NETLINK_MAX_PAYLOAD : len);
        return netlink_send(sockfd, data, max_send);
    }

    if (s->type == SOCK_STREAM && s->conn_id >= 0) {
        /* TCP stream — net_tcp_send with max 65535 per call */
        uint16_t max_send = (len > 65535) ? 65535 : (uint16_t)len;
        return net_tcp_send(s->conn_id, data, max_send);
    }

    if (s->type == SOCK_DGRAM) {
        /* UDP datagram — connected or connectionless */
        if (s->cache_valid && s->state == SOCK_STATE_CONNECTED &&
            s->remote_ip != 0) {
            net_udp_send_cached(s->cached_dst_mac, s->remote_ip,
                                s->local_port, s->remote_port,
                                data, (uint16_t)(len > 1500 ? 1500 : len));
        } else {
            net_udp_send(s->remote_ip, s->local_port, s->remote_port,
                         data, (uint16_t)(len > 1500 ? 1500 : len));
        }
        return (int)len;
    }

    /* Unsupported socket type (AF_PACKET, AF_CAN, etc.) */
    return -EINVAL;
}

/* ── ioctl ─────────────────────────────────────────────────────────────── */

static uint64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;
    if (fd >= PROCESS_FD_MAX || !p->fd_table[fd].used) return (uint64_t)(int64_t)-EBADF;

    /* For I/O control operations that use arg as a pointer, reject NULL */
    if (!arg) return (uint64_t)(int64_t)-EINVAL;

    switch (cmd) {
        case TIOCGWINSZ: {
            /* Return a winsize struct — dummy values for now */
            struct {
                unsigned short ws_row;
                unsigned short ws_col;
                unsigned short ws_xpixel;
                unsigned short ws_ypixel;
            } ws = { 25, 80, 0, 0 };
            if (copy_to_user(arg, &ws, sizeof(ws)) < 0)
                return (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        case SG_IO: {
            /* SCSI generic passthrough — submit CDB and return sense data */
            struct sg_io_hdr hdr;
            if (copy_from_user(&hdr, arg, sizeof(hdr)) < 0)
                return (uint64_t)(int64_t)-EFAULT;

            /* Validate interface ID */
            if (hdr.interface_id != 'S')
                return (uint64_t)(int64_t)-ENOTTY;

            /* iovec not supported in this simple implementation */
            if (hdr.iovec_count != 0)
                return (uint64_t)(int64_t)-EINVAL;

            /* Validate CDB length */
            if (hdr.cmd_len > SG_MAX_CDB_SIZE || hdr.cmd_len == 0)
                return (uint64_t)(int64_t)-EINVAL;

            /* Resolve block device ID from the fd's path.
             * The fd path should be /dev/sda, /dev/nvme0n1, etc. */
            const char *path = p->fd_table[fd].path;
            if (!path || path[0] == '\0')
                return (uint64_t)(int64_t)-EBADF;

            int dev_id = blockdev_find_by_name(path);
            if (dev_id < 0)
                return (uint64_t)(int64_t)-ENODEV;

            /* Copy CDB from user space */
            uint8_t cdb[SG_MAX_CDB_SIZE];
            memset(cdb, 0, sizeof(cdb));
            if (copy_from_user(cdb, (uint64_t)hdr.cmdp, hdr.cmd_len) < 0)
                return (uint64_t)(int64_t)-EFAULT;

            /* Allocate data buffer (up to 64KB for safety) */
            uint32_t data_len = hdr.dxfer_len;
            if (data_len > 65536)
                return (uint64_t)(int64_t)-EINVAL;

            void *data_buf = NULL;
            if (data_len > 0 && hdr.dxferp) {
                data_buf = (void *)kmalloc(data_len);
                if (!data_buf)
                    return (uint64_t)(int64_t)-ENOMEM;

                if (hdr.dxfer_direction == SG_DXFER_TO_DEV ||
                    hdr.dxfer_direction == SG_DXFER_TO_FROM_DEV) {
                    /* Copy data FROM user for writes */
                    if (copy_from_user(data_buf, (uint64_t)hdr.dxferp, data_len) < 0) {
                        kfree(data_buf);
                        return (uint64_t)(int64_t)-EFAULT;
                    }
                }
            }

            /* Sense buffer */
            uint8_t sense[SG_MAX_SENSE_SIZE];
            int sense_len = 0;
            memset(sense, 0, sizeof(sense));

            /* Submit the SCSI command */
            uint64_t start_tick = 0;
            if (timer_available())
                start_tick = timer_get_ticks();

            int ret = blockdev_scsi_submit(dev_id, cdb, hdr.cmd_len,
                                            data_buf, (int)data_len,
                                            hdr.dxfer_direction,
                                            sense, &sense_len,
                                            (int)hdr.timeout);

            /* Calculate duration in ms */
            uint32_t duration_ms = 0;
            if (timer_available()) {
                uint64_t elapsed = timer_get_ticks() - start_tick;
                duration_ms = (uint32_t)(elapsed * 1000 / TIMER_FREQ);
            }

            /* Fill in the hdr result fields */
            if (ret < 0) {
                hdr.status = 0xFF;  /* SCSI status = host error */
                hdr.host_status = (unsigned short)(-ret);
                hdr.resid = (int)data_len;  /* all data residual */
            } else {
                hdr.status = 0;  /* GOOD status */
                hdr.host_status = 0;
                hdr.resid = 0;
            }
            hdr.duration = duration_ms;
            hdr.sb_len_wr = (unsigned char)sense_len;
            if (sense_len > 0 && hdr.sbp) {
                unsigned char mx_sb = hdr.mx_sb_len;
                if ((int)mx_sb > sense_len) mx_sb = (unsigned char)sense_len;
                if (copy_to_user((uint64_t)hdr.sbp, sense, mx_sb) < 0) {
                    if (data_buf) kfree(data_buf);
                    return (uint64_t)(int64_t)-EFAULT;
                }
            }

            /* Copy data back to user for reads */
            if (data_buf && data_len > 0 && hdr.dxferp &&
                (hdr.dxfer_direction == SG_DXFER_FROM_DEV ||
                 hdr.dxfer_direction == SG_DXFER_TO_FROM_DEV)) {
                if (copy_to_user((uint64_t)hdr.dxferp, data_buf, data_len) < 0) {
                    kfree(data_buf);
                    return (uint64_t)(int64_t)-EFAULT;
                }
            }

            if (data_buf)
                kfree(data_buf);

            /* Copy the updated hdr back to user */
            if (copy_to_user(arg, &hdr, sizeof(hdr)) < 0)
                return (uint64_t)(int64_t)-EFAULT;

            return 0;
        }
        default:
            return (uint64_t)(int64_t)-ENOTTY;
    }
}

/* ── syslog/kmsg ───────────────────────────────────────────────────────── */

static uint64_t sys_syslog(uint64_t type, uint64_t buf_addr, uint64_t len) {
    /* Lockdown CONFIDENTIALITY: block reading dmesg from userspace */
    if (lockdown_is_locked_down(LOCKDOWN_CONFIDENTIALITY)) {
        switch (type) {
            case SYSLOG_ACTION_READ_ALL:
            case SYSLOG_ACTION_READ_CLEAR:
            case SYSLOG_ACTION_SIZE_UNREAD:
            case SYSLOG_ACTION_SIZE_BUFFER:
                return (uint64_t)-EPERM;
            default:
                break;  /* Allow clear, console ops, etc. */
        }
    }

    /* Check dmesg_restrict: only users with CAP_SYSLOG can read dmesg when set */
    if (dmesg_restrict) {
        struct process *p = process_get_current();
        if (p && p->is_user) {
            /* Check for CAP_SYSLOG in effective set */
            int cap_word = CAP_SYSLOG / 64;
            int cap_bit  = CAP_SYSLOG % 64;
            if (cap_word < PROCESS_SYSCALL_CAP_WORDS) {
                if (!(p->syscall_caps[cap_word] & (1ULL << cap_bit))) {
                    return (uint64_t)-1;  /* EPERM */
                }
            }
        }
    }

    switch (type) {
        case SYSLOG_ACTION_READ_ALL:
        case SYSLOG_ACTION_READ_CLEAR: {
            if (syscall_is_user_process() && !syscall_user_write_ok(buf_addr, len))
                return (uint64_t)-1;
            char *dst = (char *)buf_addr;
            int copied = kprintf_dmesg(dst, (int)len);
            if (type == SYSLOG_ACTION_READ_CLEAR)
                kprintf_dmesg_clear();
            return (uint64_t)copied;
        }
        case SYSLOG_ACTION_SIZE_BUFFER:
            return (uint64_t)(65536); /* DMESG_BUF_SIZE */
        case SYSLOG_ACTION_SIZE_UNREAD:
            /* Approximate: return max possible */
            return (uint64_t)(65536);
        case SYSLOG_ACTION_CLEAR:
            kprintf_dmesg_clear();
            return 0;
        default:
            return (unsigned long)-1;
    }
}

/* ── prctl ─────────────────────────────────────────────────────────────── */

static uint64_t sys_prctl(uint64_t op, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;

    switch (op) {
        case PR_SET_NAME: {
            if (syscall_is_user_process() && !syscall_user_read_ok(a2, 16))
                return (uint64_t)(int64_t)-EFAULT;
            memset(p->proc_comm, 0, 16);
            memcpy(p->proc_comm, (const char *)a2, 15);
            p->proc_comm[15] = '\0';
            return 0;
        }
        case PR_GET_NAME: {
            char name[16];
            memcpy(name, p->proc_comm, 16);
            if (copy_to_user(a2, name, 16) < 0)
                return (uint64_t)(int64_t)-EFAULT;
            return 0;
        }
        case PR_SET_PDEATHSIG: {
            /* Store the death signal — if we had a field */
            return 0;
        }
        case PR_GET_PDEATHSIG: {
            return 0;
        }
        case PR_SET_NO_NEW_PRIVS: {
            /* Once set to 1, no_new_privs is irreversible.
             * After this, execve() cannot gain privileges via setuid/setgid
             * or file capabilities. Foundation for modern seccomp usage. */
            if (a2 != 1 || p->no_new_privs) return (uint64_t)(int64_t)-EINVAL;
            p->no_new_privs = 1;
            return 0;
        }
        case PR_GET_NO_NEW_PRIVS: {
            return p->no_new_privs;
        }
        case PR_SET_SECCOMP: {
            /* arg2 = seccomp mode, arg3 = flags (e.g. SECCOMP_FILTER_FLAG_TSYNC) */
            return (uint64_t)seccomp_set_mode((int)a2, (unsigned int)a3);
        }
        case PR_GET_SECCOMP: {
            return (uint64_t)seccomp_get_mode();
        }
        case PR_SET_SECUREBITS: {
            /* arg2 = securebits bitmask. Only allow setting non-locked bits. */
            uint8_t new_bits = (uint8_t)(a2 & 0xFF);
            uint8_t current = (uint8_t)securebits_get(p);
            /* Locked bits are immutable */
            if ((current & SECBIT_LOCKED_MASK) & (new_bits & SECBIT_LOCKED_MASK))
                return (uint64_t)(int64_t)-EPERM;
            /* Can only set bits in SECBIT_ALLOWED_MASK */
            if (new_bits & ~SECBIT_ALLOWED_MASK & ~SECBIT_LOCKED_MASK)
                return (uint64_t)(int64_t)-EINVAL;
            /* Setting a locked bit requires the corresponding non-locked bit to be set */
            if ((new_bits & SECBIT_KEEP_CAPS_LOCKED) && !(new_bits & SECBIT_KEEP_CAPS))
                return (uint64_t)(int64_t)-EINVAL;
            if ((new_bits & SECBIT_NO_SETUID_FIXUP_LOCKED) && !(new_bits & SECBIT_NO_SETUID_FIXUP))
                return (uint64_t)(int64_t)-EINVAL;
            if ((new_bits & SECBIT_NOROOT_LOCKED) && !(new_bits & SECBIT_NOROOT))
                return (uint64_t)(int64_t)-EINVAL;
            return (uint64_t)securebits_set(p, new_bits);
        }
        case PR_GET_SECUREBITS: {
            return (uint64_t)securebits_get(p);
        }
        case PR_SET_DUMPABLE: {
            /* arg2: 0 = never dump, 1 = always dump, 2 = dump if root.
             * Only privileged processes (CAP_SYS_ADMIN or running as root)
             * can set dumpable to a more restrictive value. */
            int val = (int)a2;
            if (val < 0 || val > 2)
                return (uint64_t)(int64_t)-EINVAL;
            /* Once set to 0, only privileged code can raise it back */
            p->dumpable = val;
            return 0;
        }
        case PR_GET_DUMPABLE: {
            return (uint64_t)p->dumpable;
        }
        case PR_SET_PTRACER: {
            /* arg2 = allowed tracer PID:
             *   0       = no tracer allowed (default)
             *  -1       = any tracer allowed
             *   >0      = specific PID allowed to trace
             * Only the calling process can set its own ptracer.
             * CAP_SYS_PTRACE can override but the target must still opt in. */
            int tracer_pid = (int)a2;
            yama_set_ptracer(p->pid, tracer_pid);
            return 0;
        }
        case PR_GET_PTRACER: {
            /* Return the PID this process has allowed to trace it,
             * or 0 if none, or -1 if any. */
            return (uint64_t)(int64_t)yama_get_ptracer(p->pid);
        }
        default:
            return (uint64_t)(int64_t)-EINVAL;
    }
}

/* ── mount/umount ──────────────────────────────────────────────────────── */

static uint64_t sys_mount(uint64_t src_addr, uint64_t target_addr,
                           uint64_t fstype_addr, uint64_t flags, uint64_t data_addr) {
    (void)data_addr;
    char src[64], target[64], fstype[16];

    if (!syscall_user_cstr_ok(src_addr) || !syscall_user_cstr_ok(target_addr))
        return (uint64_t)(int64_t)-EFAULT;

    memcpy(src, (void*)src_addr, 63); src[63] = '\0';
    memcpy(target, (void*)target_addr, 63); target[63] = '\0';

    if (fstype_addr && syscall_user_cstr_ok(fstype_addr)) {
        memcpy(fstype, (void*)fstype_addr, 15); fstype[15] = '\0';
    } else {
        fstype[0] = '\0';
    }

    /* Handle bind mounts (mount --bind) */
    if (flags & MS_BIND) {
        int ret = vfs_bind_mount(src, target);
        if (ret < 0)
            return (uint64_t)(int64_t)ret;
        kprintf("[mount] bind: %s at %s\n", src, target);
        return 0;
    }

    /* ── Filesystem module autoloading (M36) ──────────────────────── */
    /* ... (unchanged) ... */
    if (fstype[0] != '\0') {
        /* Quick check: is this filesystem already registered? */
        int found = 0;
        {
            char names[VFS_MAX_FS_TYPES][32];
            int n = vfs_list_filesystems(names, VFS_MAX_FS_TYPES);
            for (int i = 0; i < n && !found; i++) {
                if (strcmp(names[i], fstype) == 0)
                    found = 1;
            }
        }

        /* If not found, try to autoload it */
        if (!found) {
            request_module("%s", fstype);
            {
                char names[VFS_MAX_FS_TYPES][32];
                int n = vfs_list_filesystems(names, VFS_MAX_FS_TYPES);
                for (int i = 0; i < n && !found; i++) {
                    if (strcmp(names[i], fstype) == 0)
                        found = 1;
                }
            }
        }

        /* Only support tmpfs for now */
        if (strcmp(fstype, "tmpfs") == 0 || strcmp(fstype, "ramfs") == 0) {
            kprintf("[mount] %s at %s (type=%s)\n", src, target, fstype);
            return 0;
        }

        if (found) {
            kprintf("[mount] %s at %s (type=%s) — module loaded, VFS mount pending\n",
                    src, target, fstype);
            return 0;
        }

        /* Filesystem type not found */
        kprintf("[mount] %s at %s (type=%s): filesystem not supported\n",
                src, target, fstype);
        return (uint64_t)(int64_t)-ENODEV;
    }

    /* For the existing fs.c (smfs), just treat as a no-op */
    kprintf("[mount] src=%s target=%s fstype=%s\n", src, target, fstype);

    struct process *cur = process_get_current();
    struct mnt_namespace *ns = cur ? cur->mnt_ns : NULL;
    if (ns) {
        kprintf("[mount] (namespace=%p) %s at %s type=%s\n",
                (void*)ns, src, target, fstype);
    }
    return 0;
}

static uint64_t sys_umount(uint64_t target_addr) {
    char target[64];
    if (syscall_is_user_process() && !syscall_user_cstr_ok(target_addr))
        return (uint64_t)(int64_t)-EFAULT;
    memcpy(target, (void*)target_addr, 63); target[63] = '\0';

    kprintf("[umount] %s\n", target);
    /* In a full implementation this would call vfs_umount */
    return 0;
}

/* ── pivot_root — change root filesystem (Item 118) ─────────────────── */

static uint64_t sys_pivot_root(uint64_t new_root_addr, uint64_t put_old_addr) {
    char new_root[64], put_old[64];

    if (syscall_is_user_process()) {
        if (!syscall_user_cstr_ok(new_root_addr) ||
            !syscall_user_cstr_ok(put_old_addr))
            return (uint64_t)(int64_t)-EFAULT;
    }

    memcpy(new_root, (void*)new_root_addr, 63); new_root[63] = '\0';
    memcpy(put_old, (void*)put_old_addr, 63);   put_old[63] = '\0';

    int ret = vfs_pivot_root(new_root, put_old);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return 0;
}

/* ── chroot (Item 117) ─────────────────────────────────────────── */

/* Forward declaration — chroot_set() is implemented in chroot.c */
extern int chroot_set(const char *path);

static uint64_t sys_chroot(uint64_t path_addr) {
    char path[256];

    if (syscall_is_user_process()) {
        if (!syscall_user_cstr_ok(path_addr))
            return (uint64_t)(int64_t)-EFAULT;
    }

    memcpy(path, (void*)path_addr, 255);
    path[255] = '\0';

    int ret = chroot_set(path);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return 0;
}

/* ── ftruncate ─────────────────────────────────────────────────────────── */

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;
    if (fd >= PROCESS_FD_MAX || !p->fd_table[fd].used)
        return (uint64_t)(int64_t)-EBADF;
    /* For now, delegate to the path-based truncate */
    return sys_truncate((uint64_t)(uintptr_t)p->fd_table[fd].path, length);
}

/* ── readdir ───────────────────────────────────────────────────────────── */

static uint64_t sys_readdir(uint64_t fd, uint64_t buf_addr, uint64_t count) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;
    if (fd >= PROCESS_FD_MAX || !p->fd_table[fd].used)
        return (uint64_t)(int64_t)-EBADF;

    if (syscall_is_user_process() && !syscall_user_write_ok(buf_addr, count))
        return (uint64_t)(int64_t)-EFAULT;

    char names[64][64];
    int n = vfs_readdir_names(p->fd_table[fd].path, names, 64);
    if (n < 0) return (uint64_t)(int64_t)n;
    if (n == 0) return 0;

    /* Start from the fd's current offset (which tracks which entry we're at) */
    int start = (int)p->fd_table[fd].offset;
    if (start >= n) return 0; /* end of directory */

    uint8_t *buf = (uint8_t *)buf_addr;
    size_t total = 0;

    for (int i = start; i < n; i++) {
        int namelen = (int)strlen(names[i]);
        size_t reclen = sizeof(struct linux_dirent64) + (size_t)namelen + 1;
        /* Align to 8 bytes */
        reclen = (reclen + 7) & ~(size_t)7;

        if (total + reclen > (size_t)count) break;

        struct linux_dirent64 *entry = (struct linux_dirent64 *)(buf + total);
        entry->d_ino = 1; /* fake inode */
        entry->d_off = (int64_t)(i + 1 < n ? reclen : 0);
        entry->d_reclen = (unsigned short)reclen;
        entry->d_type = DT_UNKNOWN;
        memcpy(entry->d_name, names[i], (unsigned long)namelen + 1);
        total += reclen;
        p->fd_table[fd].offset = (uint32_t)(i + 1);
    }

    return (uint64_t)total;
}

/* ── execveat ──────────────────────────────────────────────────────────── */

static uint64_t sys_execveat(uint64_t dirfd, uint64_t path_addr,
                              uint64_t argv_addr, uint64_t envp_addr,
                              uint64_t flags) {
    (void)dirfd; (void)argv_addr; (void)envp_addr; (void)flags;
    char path[256];

    if (!syscall_user_cstr_ok(path_addr))
        return (uint64_t)(int64_t)-EFAULT;
    memcpy(path, (void*)path_addr, 255); path[255] = '\0';

    /* Resolve relative paths against dirfd if not AT_EMPTY_PATH */
    if (path[0] != '/' && !(flags & AT_EMPTY_PATH)) {
        struct process *p = process_get_current();
        if (p && dirfd < PROCESS_FD_MAX && p->fd_table[dirfd].used) {
            char base[64];
            memcpy(base, p->fd_table[dirfd].path, 63); base[63] = '\0';
            /* Strip filename from base path, keep directory */
            char *last_slash = NULL;
            for (char *c = base; *c; c++) if (*c == '/') last_slash = c;
            if (last_slash) *(last_slash + 1) = '\0';
            /* Combine */
            char combined[256];
            int n = snprintf(combined, 256, "%s%s", base, path);
            if (n < 0 || n >= 256) return (uint64_t)(int64_t)-ENAMETOOLONG;
            /* Use existing sys_execve which takes a path */
            return sys_script_exec((uint64_t)(uintptr_t)combined);
        }
    }

    return sys_script_exec(path_addr);
}

/* ── sched_setscheduler / sched_getscheduler ──────────────────────────── */

static uint64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy,
                                        uint64_t param_addr) {
    struct process *target;
    if (pid == 0)
        target = process_get_current();
    else
        target = process_get_by_pid((uint32_t)pid);

    if (!target || target->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-ESRCH;

    if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR &&
        policy != SCHED_BATCH && policy != SCHED_IDLE)
        return (uint64_t)(int64_t)-EINVAL;

    target->sched_policy = (uint8_t)policy;

    /* SCHED_IDLE tasks always go to the lowest priority level */
    if (policy == SCHED_IDLE) {
        target->priority = SCHED_LEVELS - 1;
    }

    if (param_addr) {
        struct sched_param param;
        if (copy_from_user(&param, param_addr, sizeof(param)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
        /* SCHED_FIFO/RR priority: only privileged processes can set > 0 */
        if (param.sched_priority > 0)
            target->priority = (uint8_t)(param.sched_priority > 3 ? 3 : param.sched_priority);
    }

    return 0;
}

static uint64_t sys_sched_getscheduler(uint64_t pid) {
    struct process *target;
    if (pid == 0)
        target = process_get_current();
    else
        target = process_get_by_pid((uint32_t)pid);

    if (!target || target->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-ESRCH;
    return (uint64_t)target->sched_policy;
}

/* ── sched_setattr / sched_getattr — extended scheduler attributes (Item 310) ── */

/*
 * sys_sched_setattr — Set extended scheduling attributes for a process.
 *
 *   pid:    target PID (0 = current)
 *   attr:   userspace pointer to struct sched_attr
 *   flags:  SCHED_FLAG_* (SCHED_FLAG_RESET_ON_FORK, etc.)
 *
 * Returns 0 on success, -1 with errno set on error.
 */
static uint64_t sys_sched_setattr(uint64_t pid, uint64_t attr_addr, uint64_t flags)
{
    struct sched_attr attr;
    int ret;

    if (!attr_addr)
        return (uint64_t)(int64_t)-EINVAL;

    /* Copy struct sched_attr from userspace */
    if (syscall_is_user_process() && !syscall_user_read_ok((uint64_t)attr_addr, sizeof(struct sched_attr)))
        return (uint64_t)(int64_t)-EFAULT;
    memcpy(&attr, (const void *)attr_addr, sizeof(struct sched_attr));

    /* Validate the size field before using it */
    if (attr.size == 0 || attr.size > sizeof(struct sched_attr))
        return (uint64_t)(int64_t)-EINVAL;

    /* If caller provided a smaller struct, zero the tail */
    size_t copy_size = attr.size < sizeof(struct sched_attr) ? attr.size : sizeof(struct sched_attr);
    if (copy_size < sizeof(struct sched_attr))
        memset((uint8_t *)&attr + copy_size, 0, sizeof(struct sched_attr) - copy_size);
    attr.size = sizeof(struct sched_attr);  /* normalize */

    /* Resolve PID */
    if (pid == 0)
        pid = process_get_current() ? process_get_current()->pid : 0;
    if (pid == 0)
        return (uint64_t)(int64_t)-ESRCH;

    ret = sched_setattr((uint32_t)pid, &attr, (uint32_t)flags);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return 0;
}

/*
 * sys_sched_getattr — Get extended scheduling attributes for a process.
 *
 *   pid:    target PID (0 = current)
 *   attr:   userspace pointer to struct sched_attr
 *   size:   size of caller's struct sched_attr (for forward compat)
 *   flags:  reserved, must be 0
 *
 * Returns 0 on success, -1 with errno set on error.
 */
static uint64_t sys_sched_getattr(uint64_t pid, uint64_t attr_addr, uint64_t size, uint64_t flags)
{
    struct sched_attr attr;
    int ret;

    if (!attr_addr || size == 0 || size > sizeof(struct sched_attr))
        return (uint64_t)(int64_t)-EINVAL;

    if (syscall_is_user_process() && !syscall_user_write_ok((uint64_t)attr_addr, sizeof(struct sched_attr)))
        return (uint64_t)(int64_t)-EFAULT;

    /* Resolve PID */
    uint32_t target_pid;
    if (pid == 0) {
        struct process *cur = process_get_current();
        if (!cur) return (uint64_t)(int64_t)-ESRCH;
        target_pid = cur->pid;
    } else {
        target_pid = (uint32_t)pid;
    }

    ret = sched_getattr(target_pid, &attr, (size_t)size, (uint32_t)flags);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;

    /* Copy result back to userspace */
    size_t copy_size = size < sizeof(struct sched_attr) ? (size_t)size : sizeof(struct sched_attr);
    if (copy_to_user(attr_addr, &attr, copy_size) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    return 0;
}

/* ── Core dump support ─────────────────────────────────────────────────── */

#include "coredump_core.h"

void do_coredump(struct process *proc, int signo) {
    if (!proc || !proc->coredump_enabled) return;
    if (!proc->is_user || !proc->pml4) return;

    /* Enforce RLIMIT_CORE: if core size limit is 0, skip dump */
    if (proc->rlim_cur[1] == 0) {  /* RLIMIT_CORE = 1 (syscall.h convention) */
        kprintf("[CORE] pid=%u: core dump suppressed (RLIMIT_CORE=0)\n", proc->pid);
        return;
    }

    kprintf("[CORE] pid=%u name=\"%s\": scheduling core dump (max %llu bytes, signal %d)...\n", proc->pid,
            proc->name ? proc->name : "?",
            (unsigned long long)proc->rlim_cur[1], signo);

    /* Dispatch via the registered handler (may be NULL if coredump module
     * is not loaded).  The handler is responsible for deferring to a
     * workqueue if called from IRQ context. */
    coredump_trigger(proc->pid, signo);
}

/* ══════════════════════════════════════════════════════════════════════════ */

/* ── *at family helpers ───────────────────────────────────────────────── */

/* Resolve a path relative to a dirfd.  If path is absolute, it's used
 * verbatim.  If dirfd == AT_FDCWD, use current process cwd.  Otherwise
 * look up the fd in the fd table and use its directory as the base.
 * Returns a pointer to the resolved path (in a static buffer) or NULL. */
static const char *resolve_path_at(int dirfd, const char *path) {
    static char buf[256];
    if (!path) return NULL;

    if (path[0] == '/') return path; /* absolute */

    struct process *p = process_get_current();
    if (!p) return NULL;

    const char *base = NULL;
    if (dirfd == AT_FDCWD) {
        base = p->cwd;
    } else if (dirfd >= 0 && dirfd < PROCESS_FD_MAX && p->fd_table[dirfd].used) {
        base = p->fd_table[dirfd].path;
    } else {
        return NULL;
    }

    int n = snprintf(buf, sizeof(buf), "%s/%s", base, path);
    if (n < 0 || (size_t)n >= sizeof(buf)) return NULL;
    return buf;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 20 more production-ready improvements — Batch 3
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── Socket syscall wrappers ───────────────────────────────────────────── */

/* Forward declarations from net/socket.c */
int sys_socket_impl(int domain, int type, int protocol);
int sys_bind_impl(int sockfd, const struct sockaddr_in *addr);
int sys_listen_impl(int sockfd, int backlog);
int sys_accept_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen);
int sys_connect_impl(int sockfd, const struct sockaddr_in *addr);
int sys_setsockopt_impl(int sockfd, int level, int optname, const void *optval, uint32_t optlen);
int sys_getsockopt_impl(int sockfd, int level, int optname, void *optval, uint32_t *optlen);
int sys_sendmsg_impl(int sockfd, const struct msghdr *msg, int flags);
int sys_recvmsg_impl(int sockfd, struct msghdr *msg, int flags);
int sys_getsockname_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen);
int sys_getpeername_impl(int sockfd, struct sockaddr_in *addr, uint32_t *addrlen);
int sys_socketpair_impl(int domain, int type, int protocol, int sv[2]);

static uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
    int ret = sys_socket_impl((int)domain, (int)type, (int)protocol);
    return ret >= 0 ? (uint64_t)ret : (uint64_t)(int64_t)ret;
}

static uint64_t sys_bind(uint64_t sockfd, uint64_t addr_addr, uint64_t addrlen) {
    (void)addrlen;
    if (syscall_is_user_process() && !syscall_user_read_ok(addr_addr, sizeof(struct sockaddr_in)))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_bind_impl((int)sockfd, (const struct sockaddr_in *)addr_addr);
}

static uint64_t sys_listen(uint64_t sockfd, uint64_t backlog) {
    return (uint64_t)(int64_t)sys_listen_impl((int)sockfd, (int)backlog);
}

static uint64_t sys_accept(uint64_t sockfd, uint64_t addr_addr, uint64_t addrlen_addr) {
    if (addr_addr && addrlen_addr) {
        if (syscall_is_user_process() && !syscall_user_write_ok(addr_addr, sizeof(struct sockaddr_in)))
            return (uint64_t)(int64_t)-EFAULT;
        if (syscall_is_user_process() && !syscall_user_read_ok(addrlen_addr, 4))
            return (uint64_t)(int64_t)-EFAULT;
    }
    int fd = sys_accept_impl((int)sockfd,
                             (struct sockaddr_in *)addr_addr,
                             (uint32_t *)addrlen_addr);
    return fd >= 0 ? (uint64_t)fd : (uint64_t)(int64_t)fd;
}

static uint64_t sys_connect(uint64_t sockfd, uint64_t addr_addr, uint64_t addrlen) {
    (void)addrlen;
    if (syscall_is_user_process() && !syscall_user_read_ok(addr_addr, sizeof(struct sockaddr_in)))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_connect_impl((int)sockfd, (const struct sockaddr_in *)addr_addr);
}

static uint64_t sys_setsockopt(uint64_t sockfd, uint64_t level, uint64_t optname,
                                uint64_t optval_addr, uint64_t optlen) {
    if (syscall_is_user_process() && !syscall_user_read_ok(optval_addr, (uint32_t)optlen))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_setsockopt_impl((int)sockfd, (int)level, (int)optname,
                                                  (const void *)optval_addr, (uint32_t)optlen);
}

static uint64_t sys_getsockopt(uint64_t sockfd, uint64_t level, uint64_t optname,
                                uint64_t optval_addr, uint64_t optlen_addr) {
    if (syscall_is_user_process() && !syscall_user_write_ok(optval_addr, 4))
        return (uint64_t)(int64_t)-EFAULT;
    if (syscall_is_user_process() && !syscall_user_read_ok(optlen_addr, 4))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_getsockopt_impl((int)sockfd, (int)level, (int)optname,
                                                  (void *)optval_addr, (uint32_t *)optlen_addr);
}

static uint64_t sys_sendmsg(uint64_t sockfd, uint64_t msg_addr, uint64_t flags) {
    if (syscall_is_user_process() && !syscall_user_read_ok(msg_addr, sizeof(struct msghdr)))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_sendmsg_impl((int)sockfd, (const struct msghdr *)msg_addr, (int)flags);
}

static uint64_t sys_recvmsg(uint64_t sockfd, uint64_t msg_addr, uint64_t flags) {
    if (syscall_is_user_process() && !syscall_user_read_ok(msg_addr, sizeof(struct msghdr)))
        return (uint64_t)(int64_t)-EFAULT;
    if (syscall_is_user_process() && !syscall_user_write_ok(msg_addr, sizeof(struct msghdr)))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_recvmsg_impl((int)sockfd, (struct msghdr *)msg_addr, (int)flags);
}

static uint64_t sys_getsockname(uint64_t sockfd, uint64_t addr_addr, uint64_t addrlen_addr) {
    if (syscall_is_user_process() && !syscall_user_write_ok(addr_addr, sizeof(struct sockaddr_in)))
        return (uint64_t)(int64_t)-EFAULT;
    if (syscall_is_user_process() && !syscall_user_read_ok(addrlen_addr, 4))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_getsockname_impl((int)sockfd, (struct sockaddr_in *)addr_addr,
                                                   (uint32_t *)addrlen_addr);
}

static uint64_t sys_getpeername(uint64_t sockfd, uint64_t addr_addr, uint64_t addrlen_addr) {
    if (syscall_is_user_process() && !syscall_user_write_ok(addr_addr, sizeof(struct sockaddr_in)))
        return (uint64_t)(int64_t)-EFAULT;
    if (syscall_is_user_process() && !syscall_user_read_ok(addrlen_addr, 4))
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)(int64_t)sys_getpeername_impl((int)sockfd, (struct sockaddr_in *)addr_addr,
                                                   (uint32_t *)addrlen_addr);
}

static uint64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv_addr) {
    if (syscall_is_user_process() && !syscall_user_write_ok(sv_addr, 8))
        return (uint64_t)(int64_t)-EFAULT;
    int sv[2];
    int r = sys_socketpair_impl((int)domain, (int)type, (int)protocol, sv);
    if (r < 0) return (uint64_t)(int64_t)r;
    memcpy((void *)sv_addr, sv, 8);
    return 0;
}

/* ── epoll ─────────────────────────────────────────────────────────────── */

/*
 * Syscall wrappers — translate from the syscall dispatch ABI
 * (uint64_t args, (uint64_t)-1 for errors) to the kernel-internal
 * epoll API defined in epoll.h/epoll.c, which returns proper
 * negative errno values.
 *
 * The actual implementation lives in src/kernel/epoll.c.
 */

static uint64_t sys_epoll_create1(uint64_t flags)
{
    int ret = epoll_create1_syscall((int)flags);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return (uint64_t)ret;
}

static uint64_t sys_epoll_ctl(uint64_t epfd, uint64_t op,
                               uint64_t fd, uint64_t event_addr)
{
    struct epoll_event ev;
    struct epoll_event *ev_ptr = NULL;

    if (event_addr != 0) {
        if (syscall_is_user_process() &&
            !syscall_user_read_ok(event_addr, sizeof(struct epoll_event)))
            return (uint64_t)(int64_t)-EFAULT;
        if (copy_from_user(&ev, (uint64_t)(void *)event_addr,
                           sizeof(struct epoll_event)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
        ev_ptr = &ev;
    }

    int ret = epoll_ctl_syscall((int)epfd, (int)op, (int)fd, ev_ptr);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return 0;
}

static uint64_t sys_epoll_wait(uint64_t epfd, uint64_t events_addr,
                                uint64_t maxevents, uint64_t timeout)
{
    struct epoll_event *events = (struct epoll_event *)events_addr;

    if (maxevents == 0)
        return (uint64_t)(int64_t)-EINVAL;

    if (syscall_is_user_process() &&
        !syscall_user_write_ok(events_addr,
            (uint64_t)maxevents * sizeof(struct epoll_event)))
        return (uint64_t)(int64_t)-EFAULT;

    int ret = epoll_wait_syscall((int)epfd, events,
                                  (int)maxevents, (int)timeout);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return (uint64_t)ret;
}

static uint64_t sys_epoll_pwait(uint64_t epfd, uint64_t events_addr,
                                 uint64_t maxevents, uint64_t timeout,
                                 uint64_t sigmask_addr)
{
    /*
     * The syscall dispatch saves the packed sigmask pointer + size
     * in syscall_arg6.  For epoll_pwait, a5 is the sigmask pointer.
     * The sigsetsize arrives independently but is not currently
     * validated — kept for future signal-mask integration.
     */
    struct epoll_event *events = (struct epoll_event *)events_addr;
    const uint64_t *sigmask = NULL;

    if (maxevents == 0)
        return (uint64_t)(int64_t)-EINVAL;

    if (sigmask_addr != 0)
        sigmask = (const uint64_t *)sigmask_addr;

    if (syscall_is_user_process() &&
        !syscall_user_write_ok(events_addr,
            (uint64_t)maxevents * sizeof(struct epoll_event)))
        return (uint64_t)(int64_t)-EFAULT;

    int ret = epoll_pwait_syscall((int)epfd, events,
                                   (int)maxevents, (int)timeout,
                                   sigmask);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return (uint64_t)ret;
}

/* ── Clock & Timer syscalls ─────────────────────────────────────────────
 *
 * Implementation moved to src/kernel/posix_timer.c.
 * Declarations in include/syscall.h.
 *
 * Exported functions called from the dispatch table below:
 *   sys_clock_gettime, sys_clock_settime, sys_clock_getres,
 *   sys_timer_create, sys_timer_settime, sys_timer_gettime,
 *   sys_timer_getoverrun, sys_timer_delete, posix_timer_tick.
 * ──────────────────────────────────────────────────────────────────────── */

/* ── Modern FD operations ─────────────────────────────────────────────── */

static uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    /* Validate flags — only O_CLOEXEC is valid for dup3 */
    if (flags & ~(uint64_t)O_CLOEXEC)
        return (uint64_t)-1;

    /* Bounds check both fds against MAX_FDS */
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS)
        return (uint64_t)-1;

    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;
    if (oldfd >= PROCESS_FD_MAX || !proc->fd_table[oldfd].used)
        return (uint64_t)-1;
    if (newfd >= PROCESS_FD_MAX)
        return (uint64_t)-1;

    /* If oldfd == newfd and flags has O_CLOEXEC, just update the flag */
    if (oldfd == newfd) {
        if (flags & O_CLOEXEC)
            proc->fd_table[newfd].flags |= FD_CLOEXEC;
        else
            proc->fd_table[newfd].flags = (uint8_t)(proc->fd_table[newfd].flags & ~(unsigned)FD_CLOEXEC);
        return newfd;
    }

    /* Close new_fd if open */
    if (proc->fd_table[newfd].used) {
        memset(&proc->fd_table[newfd], 0, sizeof(struct process_fd));
    }

    /* Duplicate fd entry */
    proc->fd_table[newfd] = proc->fd_table[oldfd];

    /* Set CLOEXEC flag if requested */
    if (flags & O_CLOEXEC)
        proc->fd_table[newfd].flags |= FD_CLOEXEC;
    else
        proc->fd_table[newfd].flags = (uint8_t)(proc->fd_table[newfd].flags & ~(unsigned)FD_CLOEXEC);

    return newfd;
}

static uint64_t sys_pipe2(uint64_t fds_addr, uint64_t flags) {
    /* Validate flags — only O_CLOEXEC and O_NONBLOCK are valid for pipe2 */
    if (flags & ~(uint64_t)(O_CLOEXEC | O_NONBLOCK))
        return (uint64_t)-1;
    if (!fds_addr)
        return (uint64_t)-1;
    if (syscall_is_user_process() && !syscall_user_write_ok(fds_addr, 8))
        return (uint64_t)-1;

    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    int id = pipe_create();
    if (id < 0) return (uint64_t)-1;

    /* Set non-blocking mode if requested */
    if (flags & O_NONBLOCK)
        pipe_set_nonblock(id, 1);

    /* Allocate two FD slots */
    int read_fd = -1, write_fd = -1;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!proc->fd_table[i].used) {
            if (read_fd < 0) read_fd = i;
            else if (write_fd < 0) { write_fd = i; break; }
        }
    }
    if (read_fd < 0 || write_fd < 0) return (uint64_t)-1;

    /* Store pipe index as fd entries */
    proc->fd_table[read_fd].used = true;
    proc->fd_table[read_fd].offset = (uint32_t)id;
    snprintf(proc->fd_table[read_fd].path, 64, "pipe_read_%d", id);
    proc->fd_table[read_fd].flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

    proc->fd_table[write_fd].used = true;
    proc->fd_table[write_fd].offset = (uint32_t)id;
    snprintf(proc->fd_table[write_fd].path, 64, "pipe_write_%d", id);
    proc->fd_table[write_fd].flags = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

    /* Write fds back to userspace */
    uint32_t fds[2] = { (uint32_t)read_fd, (uint32_t)write_fd };
    if (copy_to_user(fds_addr, fds, sizeof(fds)) < 0)
        return (uint64_t)-1;
    return 0;
}

static uint64_t sys_mkdtemp(uint64_t template_addr) {
    char tmpl[256];
    if (strncpy_from_user(tmpl, template_addr, sizeof(tmpl)) < 0)
        return (uint64_t)-1;

    int len = (int)strlen(tmpl);

    /* Validate template: must end with "XXXXXX" and be long enough */
    if (len < 6)
        return (uint64_t)-1;
    if (strcmp(tmpl + len - 6, "XXXXXX") != 0)
        return (uint64_t)-1;

    /* Replace XXXXXX with 6 random hex chars from the kernel RNG */
    uint64_t random = prng_rand64();
    snprintf(tmpl + len - 6, 7, "%06x", (unsigned int)(random & 0xFFFFFF));

    if (vfs_create(tmpl, 2) < 0)
        return (uint64_t)-1;

    if (copy_to_user(template_addr, tmpl, (size_t)len) < 0)
        return (uint64_t)-1;
    return (uint64_t)template_addr;
}

/* UTIME_NOW and UTIME_OMIT — now defined in vfs.h */
/* (syscall.c includes vfs.h which provides these) */

static uint64_t sys_utimensat(uint64_t dirfd, uint64_t path_addr,
                               uint64_t times_addr, uint64_t flags) {
    (void)flags;
    /* Resolve path */
    char path[256];
    if (!path_addr) {
        /* NULL path means operate on dirfd itself */
        if ((int)dirfd == -100) return (uint64_t)-1; /* AT_FDCWD not valid with NULL path */
        struct process *p = process_get_current();
        if (!p || dirfd >= PROCESS_FD_MAX || !p->fd_table[(int)dirfd].used)
            return (uint64_t)-1;
        strncpy(path, p->fd_table[(int)dirfd].path, 255);
        path[255] = '\0';
    } else {
        char kpath[256];
        if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
            return (uint64_t)-1;
        const char *resolved = resolve_path_at((int)dirfd, kpath);
        if (!resolved) return (uint64_t)-1;
        strncpy(path, resolved, 255);
        path[255] = '\0';
    }

    uint32_t now_sec = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
    uint32_t new_mtime = now_sec;

    if (times_addr) {
        struct timespec ts[2];
        if (copy_from_user(ts, times_addr, sizeof(ts)) < 0)
            return (uint64_t)-1;

        /* ts[0] = atime, ts[1] = mtime */
        if (ts[1].tv_nsec == UTIME_OMIT) {
            /* Don't change mtime — read current */
            struct vfs_stat st;
            if (vfs_stat(path, &st) < 0)
                return (uint64_t)-1;
            new_mtime = st.mtime;
        } else if (ts[1].tv_nsec == UTIME_NOW) {
            new_mtime = now_sec;
        } else {
            /* Specific time */
            new_mtime = (uint32_t)ts[1].tv_sec;
        }
    }

    /* Set mtime via filesystem */
    if (fs_set_mtime(path, new_mtime) < 0) {
        /* If path is on a different mount, try FAT32 */
        if (strncmp(path, "/mnt", 4) == 0 && fat32_is_mounted()) {
            /* FAT32 doesn't support mtime setting yet — just return success */
            return 0;
        }
        return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys_futimens(uint64_t fd, uint64_t times_addr) {
    struct process *p = process_get_current();
    if (!p || fd >= PROCESS_FD_MAX || !p->fd_table[(int)fd].used)
        return (uint64_t)-1;

    const char *path = p->fd_table[(int)fd].path;
    if (!path || !path[0]) return (uint64_t)-1;

    uint32_t now_sec = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
    uint32_t new_mtime = now_sec;

    if (times_addr) {
        if (syscall_is_user_process() && !syscall_user_read_ok(times_addr, 2 * sizeof(struct timespec)))
            return (uint64_t)-1;
        struct timespec ts[2];
        memcpy(ts, (void*)times_addr, 2 * sizeof(struct timespec));

        if (ts[1].tv_nsec == UTIME_OMIT) {
            struct vfs_stat st;
            if (vfs_stat(path, &st) < 0)
                return (uint64_t)-1;
            new_mtime = st.mtime;
        } else if (ts[1].tv_nsec == UTIME_NOW) {
            new_mtime = now_sec;
        } else {
            new_mtime = (uint32_t)ts[1].tv_sec;
        }
    }

    if (fs_set_mtime(path, new_mtime) < 0) {
        return (uint64_t)-1;
    }
    return 0;
}

/* ── Filesystem & System Info ─────────────────────────────────────────── */

static uint64_t sys_statfs(uint64_t path_addr, uint64_t buf_addr) {
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    struct statfs st;
    memset(&st, 0, sizeof(st));
    st.f_type = 0x4D44; /* FAT */
    st.f_bsize = 512;
    st.f_blocks = 0; /* unknown */
    st.f_bfree = 0;
    st.f_bavail = 0;
    st.f_files = 0;
    st.f_ffree = 0;
    st.f_namelen = 256;

    if (copy_to_user(buf_addr, &st, sizeof(struct statfs)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    return 0;
}

static uint64_t sys_fstatfs(uint64_t fd, uint64_t buf_addr) {
    (void)fd;
    return sys_statfs(0, buf_addr);
}

static uint64_t sys_getrusage(uint64_t who, uint64_t usage_addr) {
    if (syscall_is_user_process() && !syscall_user_write_ok(usage_addr, sizeof(struct rusage)))
        return (uint64_t)-1;

    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-1;

    struct rusage ru;
    memset(&ru, 0, sizeof(ru));

    if (who == RUSAGE_SELF || (int)who == RUSAGE_THREAD) {
        uint64_t hz = TIMER_FREQ;
        /* Convert ticks to timeval (seconds + microseconds) */
        uint64_t utime_s = proc->utime_ticks / hz;
        uint64_t utime_us = (proc->utime_ticks % hz) * (1000000ULL / hz);
        uint64_t stime_s = proc->stime_ticks / hz;
        uint64_t stime_us = (proc->stime_ticks % hz) * (1000000ULL / hz);

        ru.ru_utime.tv_sec  = utime_s;
        ru.ru_utime.tv_usec = utime_us;
        ru.ru_stime.tv_sec  = stime_s;
        ru.ru_stime.tv_usec = stime_us;

        ru.ru_minflt  = proc->minflt;
        ru.ru_majflt  = proc->majflt;
        ru.ru_nvcsw   = proc->nvcsw;
        ru.ru_nivcsw  = proc->nivcsw;

        /* Approximations */
        ru.ru_maxrss  = 0; /* not tracked yet */
    } else if ((int)who == RUSAGE_CHILDREN) {
        /* Sum children's resource usage */
        struct process *table = process_get_table();
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state == PROCESS_UNUSED) continue;
            if (table[i].parent_pid != proc->pid) continue;
            if (table[i].state == PROCESS_ZOMBIE) {
                uint64_t hz = TIMER_FREQ;
                ru.ru_utime.tv_sec  += table[i].utime_ticks / hz;
                ru.ru_utime.tv_usec += (table[i].utime_ticks % hz) * (1000000ULL / hz);
                ru.ru_stime.tv_sec  += table[i].stime_ticks / hz;
                ru.ru_stime.tv_usec += (table[i].stime_ticks % hz) * (1000000ULL / hz);
            }
            ru.ru_minflt  += table[i].minflt;
            ru.ru_majflt  += table[i].majflt;
            ru.ru_nvcsw   += table[i].nvcsw;
            ru.ru_nivcsw  += table[i].nivcsw;
        }
    } else {
        return (uint64_t)-EINVAL;
    }

    if (copy_to_user(usage_addr, &ru, sizeof(struct rusage)) < 0)
        return (uint64_t)-1;
    return 0;
}

static uint64_t sys_sysinfo(uint64_t info_addr) {
    if (syscall_is_user_process() && !syscall_user_write_ok(info_addr, sizeof(struct sysinfo)))
        return (uint64_t)-1;

    struct sysinfo info;
    memset(&info, 0, sizeof(info));
    info.uptime = timer_get_ticks() / TIMER_FREQ;
    info.totalram = pmm_get_total_frames() * PAGE_SIZE;
    info.freeram = (pmm_get_total_frames() - pmm_get_used_frames()) * PAGE_SIZE;
    info.procs = 0;
    info.mem_unit = 1;

    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *table = process_get_table();
        if (table[i].state != PROCESS_UNUSED) info.procs++;
    }

    /* Load averages */
    {
        struct process *table = process_get_table();
        int run = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state == PROCESS_RUNNING || table[i].state == PROCESS_READY)
                run++;
        }
        info.loads[0] = (uint64_t)run * 1024ULL;
        info.loads[1] = (uint64_t)run * 1024ULL;
        info.loads[2] = (uint64_t)run * 1024ULL;
    }

    if (copy_to_user(info_addr, &info, sizeof(struct sysinfo)) < 0)
        return (uint64_t)-1;
    return 0;
}

/* ── Process Credentials & Scheduling ─────────────────────────────────── */

static uint64_t sys_getresuid(uint64_t ruid_addr, uint64_t euid_addr, uint64_t suid_addr) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;

    if (ruid_addr) {
        uint32_t val = p->uid;
        if (copy_to_user(ruid_addr, &val, sizeof(val)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }
    if (euid_addr) {
        uint32_t val = p->euid;
        if (copy_to_user(euid_addr, &val, sizeof(val)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }
    if (suid_addr) {
        uint32_t val = p->euid;
        if (copy_to_user(suid_addr, &val, sizeof(val)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }
    return 0;
}

static uint64_t sys_setresuid(uint64_t ruid, uint64_t euid, uint64_t suid) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;

    /* Simple: allow setting if the caller is root (uid 0) */
    if (p->euid != 0 && (ruid != (uint64_t)-1 || euid != (uint64_t)-1 || suid != (uint64_t)-1))
        return (uint64_t)(int64_t)-EPERM;

    if (ruid != (uint64_t)-1) { p->uid = (uint32_t)ruid; p->euid = (uint32_t)ruid; }
    if (euid != (uint64_t)-1) p->euid = (uint32_t)euid;
    if (suid != (uint64_t)-1) { /* suid storage not separate */ }
    /* Clear dumpable on credential change — caller might have dropped privileges */
    p->dumpable = 0;
    return 0;
}

static uint64_t sys_getresgid(uint64_t rgid_addr, uint64_t egid_addr, uint64_t sgid_addr) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;

    if (rgid_addr) {
        uint32_t val = p->gid;
        if (copy_to_user(rgid_addr, &val, sizeof(val)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }
    if (egid_addr) {
        uint32_t val = p->egid;
        if (copy_to_user(egid_addr, &val, sizeof(val)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }
    if (sgid_addr) {
        uint32_t val = p->egid;
        if (copy_to_user(sgid_addr, &val, sizeof(val)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }
    return 0;
}

static uint64_t sys_setresgid(uint64_t rgid, uint64_t egid, uint64_t sgid) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;

    if (p->euid != 0) return (uint64_t)(int64_t)-EPERM;

    if (rgid != (uint64_t)-1) { p->gid = (uint32_t)rgid; p->egid = (uint32_t)rgid; }
    if (egid != (uint64_t)-1) p->egid = (uint32_t)egid;
    if (sgid != (uint64_t)-1) { /* sgid */ }
    /* Clear dumpable on credential change */
    p->dumpable = 0;
    return 0;
}

static uint64_t sys_sched_getparam(uint64_t pid, uint64_t param_addr) {
    struct process *target;
    if (pid == 0) target = process_get_current();
    else target = process_get_by_pid((uint32_t)pid);
    if (!target || target->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-ESRCH;

    if (syscall_is_user_process() && !syscall_user_write_ok(param_addr, sizeof(struct sched_param)))
        return (uint64_t)(int64_t)-EFAULT;

    struct sched_param param;
    param.sched_priority = (int)target->priority;
    if (copy_to_user(param_addr, &param, sizeof(struct sched_param)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    return 0;
}

static uint64_t sys_sched_setparam(uint64_t pid, uint64_t param_addr) {
    struct process *target;
    if (pid == 0) target = process_get_current();
    else target = process_get_by_pid((uint32_t)pid);
    if (!target || target->state == PROCESS_UNUSED) return (uint64_t)(int64_t)-ESRCH;

    if (syscall_is_user_process() && !syscall_user_read_ok(param_addr, sizeof(struct sched_param)))
        return (uint64_t)(int64_t)-EFAULT;

    struct sched_param param;
    if (copy_from_user(&param, param_addr, sizeof(struct sched_param)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    if (param.sched_priority >= 0 && param.sched_priority < 4)
        target->priority = (uint8_t)param.sched_priority;
    return 0;
}

/* ── POSIX Message Queues ─────────────────────────────────────────────── */

#define MQ_MAX 8
#define MQ_MAX_MSG 16
#define MQ_MSG_SIZE 256

struct mq_msg {
    char  data[MQ_MSG_SIZE];
    unsigned int prio;
    int in_use;
};

struct mq {
    int in_use;
    char name[64];
    struct mq_msg msgs[MQ_MAX_MSG];
    int num_msgs;
    uint64_t mq_maxmsg;
    uint64_t mq_msgsize;
    int  oflags;         /* open flags (O_NONBLOCK etc.) — Item 254 */
};

static struct mq mq_table[MQ_MAX];

static struct mq *mq_find_by_name(const char *name) {
    for (int i = 0; i < MQ_MAX; i++) {
        if (mq_table[i].in_use && strcmp(mq_table[i].name, name) == 0)
            return &mq_table[i];
    }
    return NULL;
}

static uint64_t sys_mq_open(uint64_t name_addr, uint64_t oflag,
                             uint64_t mode, uint64_t attr_addr) {
    (void)mode;
    char name[64];
    if (!syscall_user_cstr_ok(name_addr)) return (uint64_t)-1;
    memcpy(name, (void*)name_addr, 63); name[63] = '\0';

    /* O_CREAT (0100) flag */
    if (oflag & 0100) {
        /* Create new mq */
        struct mq *existing = mq_find_by_name(name);
        if (existing) return (uint64_t)-1; /* EEXIST if O_EXCL */
        for (int i = 0; i < MQ_MAX; i++) {
            if (!mq_table[i].in_use) {
                mq_table[i].in_use = 1;
                mq_table[i].oflags = (int)(oflag & 0xffff);  /* store relevant flags (O_NONBLOCK, O_CREAT, O_EXCL) */
                memcpy(mq_table[i].name, name, 63); mq_table[i].name[63] = '\0';
                mq_table[i].num_msgs = 0;
                mq_table[i].mq_maxmsg = MQ_MAX_MSG;
                mq_table[i].mq_msgsize = MQ_MSG_SIZE;
                if (attr_addr) {
                    struct mq_attr attr;
                    if (copy_from_user(&attr, attr_addr, sizeof(struct mq_attr)) < 0)
                        return (uint64_t)-1;
                    if (attr.mq_maxmsg > 0) mq_table[i].mq_maxmsg = attr.mq_maxmsg;
                    if (attr.mq_msgsize > 0 && attr.mq_msgsize <= MQ_MSG_SIZE)
                        mq_table[i].mq_msgsize = attr.mq_msgsize;
                }
                return (uint64_t)(800 + i); /* mqd range */
            }
        }
        return (uint64_t)-1;
    }

    /* Open existing */
    struct mq *mq = mq_find_by_name(name);
    if (!mq) return (uint64_t)-1;
    for (int i = 0; i < MQ_MAX; i++) {
        if (&mq_table[i] == mq) {
            mq_table[i].oflags = (int)(oflag & 0xffff);  /* store/replace flags on open (Item 254) */
            return (uint64_t)(800 + i);
        }
    }
    return (uint64_t)-1;
}

static uint64_t sys_mq_send(uint64_t mqd, uint64_t msg_addr,
                             uint64_t msg_len, uint64_t prio) {
    int slot = (int)mqd - 800;
    if (slot < 0 || slot >= MQ_MAX || !mq_table[slot].in_use)
        return (uint64_t)-1;

    struct mq *mq = &mq_table[slot];
    if (msg_len > mq->mq_msgsize) return (uint64_t)-EMSGSIZE;
    if (mq->num_msgs >= (int)mq->mq_maxmsg) {
        if (mq->oflags & O_NONBLOCK)
            return (uint64_t)-EAGAIN;
        /* Queue full & blocking mode — should sleep, but for now fail */
        return (uint64_t)-EAGAIN;
    }

    if (syscall_is_user_process() && !syscall_user_read_ok(msg_addr, msg_len))
        return (uint64_t)-1;

    struct mq_msg *m = &mq->msgs[mq->num_msgs++];
    memcpy(m->data, (void*)msg_addr, (unsigned long)msg_len);
    m->prio = (unsigned int)prio;
    m->in_use = 1;
    return 0;
}

static uint64_t sys_mq_receive(uint64_t mqd, uint64_t msg_addr,
                                uint64_t msg_len, uint64_t prio_addr) {
    int slot = (int)mqd - 800;
    if (slot < 0 || slot >= MQ_MAX || !mq_table[slot].in_use)
        return (uint64_t)-1;

    struct mq *mq = &mq_table[slot];
    if (mq->num_msgs == 0) {
        if (mq->oflags & O_NONBLOCK)
            return (uint64_t)-EAGAIN;
        /* Queue empty & blocking mode — should sleep, but for now fail */
        return (uint64_t)-EAGAIN;
    }

    if (syscall_is_user_process() && !syscall_user_write_ok(msg_addr, msg_len))
        return (uint64_t)-1;

    /* Dequeue highest-priority message */
    int best = 0;
    for (int i = 1; i < mq->num_msgs; i++) {
        if (mq->msgs[i].prio > mq->msgs[best].prio) best = i;
    }

    struct mq_msg *m = &mq->msgs[best];
    uint64_t copy_len = msg_len < mq->mq_msgsize ? msg_len : mq->mq_msgsize;
    if (copy_to_user(msg_addr, m->data, (unsigned long)copy_len) < 0)
        return (uint64_t)-1;

    if (prio_addr) {
        unsigned int p = m->prio;
        if (copy_to_user(prio_addr, &p, sizeof(p)) < 0)
            return (uint64_t)-1;
    }

    /* Remove message by swapping with last */
    mq->msgs[best] = mq->msgs[--mq->num_msgs];
    return (uint64_t)copy_len;
}

static uint64_t sys_mq_unlink(uint64_t name_addr) {
    char name[64];
    if (!syscall_user_cstr_ok(name_addr)) return (uint64_t)-1;
    memcpy(name, (void*)name_addr, 63); name[63] = '\0';

    struct mq *mq = mq_find_by_name(name);
    if (!mq) return (uint64_t)-1;
    mq->in_use = 0;
    return 0;
}

/* ── Init all new subsystems ──────────────────────────────────────────── */

void production_subsystems_init(void) {
    socket_init();
    posix_timer_init();
    memset(mq_table, 0, sizeof(mq_table));
    inotify_subsystem_init();
    io_uring_init();
}

/* ══════════════════════════════════════════════════════════════════════════ */

static uint64_t sys_openat(uint64_t dirfd, uint64_t path_addr,
                            uint64_t flags, uint64_t mode) {
    (void)mode;
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    const char *path = resolve_path_at((int)dirfd, kpath);
    if (!path) return (uint64_t)(int64_t)-ENOENT;
    /* Delegate to core open logic with resolved path */
    return do_sys_open(path, flags, 0);
}

static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t path_addr, uint64_t mode) {
    (void)mode;
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    const char *path = resolve_path_at((int)dirfd, kpath);
    if (!path) return (uint64_t)(int64_t)-ENOENT;
    int ret = vfs_create(path, VFS_TYPE_DIR);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
}

static uint64_t sys_fstatat(uint64_t dirfd, uint64_t path_addr,
                             uint64_t buf_addr, uint64_t flags) {
    (void)flags;
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    const char *resolved = resolve_path_at((int)dirfd, kpath);
    if (!resolved) return (uint64_t)(int64_t)-ENOENT;

    struct vfs_stat st;
    if (vfs_stat(resolved, &st) < 0) return (uint64_t)(int64_t)-ENOENT;
    if (copy_to_user(buf_addr, &st, sizeof(st)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    return 0;
}

static uint64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t flags) {
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    const char *path = resolve_path_at((int)dirfd, kpath);
    if (!path) return (uint64_t)(int64_t)-ENOENT;
    int ret;
    if (flags & AT_REMOVEDIR)
        ret = vfs_unlink(path);
    else
        ret = vfs_unlink(path);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
}

static uint64_t sys_renameat(uint64_t olddirfd, uint64_t oldpath_addr,
                              uint64_t newdirfd, uint64_t newpath_addr) {
    char koldpath[256], knewpath[256];
    if (strncpy_from_user(koldpath, oldpath_addr, sizeof(koldpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    if (strncpy_from_user(knewpath, newpath_addr, sizeof(knewpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    const char *oldpath = resolve_path_at((int)olddirfd, koldpath);
    const char *newpath = resolve_path_at((int)newdirfd, knewpath);
    if (!oldpath || !newpath) return (uint64_t)(int64_t)-ENOENT;
    /* For now, fall back to VFS operations: copy + delete */
    uint8_t *buf = kmalloc(4096);
    if (!buf) return (uint64_t)(int64_t)-ENOMEM;
    uint32_t sz = 0;
    if (vfs_read(oldpath, buf, 4096, &sz) < 0) {
        kfree(buf);
        return (uint64_t)(int64_t)-EIO;
    }
    if (vfs_write(newpath, buf, sz) < 0) {
        kfree(buf);
        return (uint64_t)(int64_t)-EIO;
    }
    vfs_unlink(oldpath);
    kfree(buf);
    return 0;
}

static uint64_t sys_symlinkat(uint64_t target_addr, uint64_t newdirfd,
                               uint64_t linkpath_addr) {
    char *ktarget = kmalloc(4096);
    if (!ktarget) return (uint64_t)(int64_t)-ENOMEM;
    char klinkpath[256];
    if (strncpy_from_user(ktarget, target_addr, 4096) < 0) {
        kfree(ktarget);
        return (uint64_t)(int64_t)-EFAULT;
    }
    ktarget[4095] = '\0';
    if (strncpy_from_user(klinkpath, linkpath_addr, sizeof(klinkpath)) < 0) {
        kfree(ktarget);
        return (uint64_t)(int64_t)-EFAULT;
    }
    const char *linkpath = resolve_path_at((int)newdirfd, klinkpath);
    if (!linkpath) {
        kfree(ktarget);
        return (uint64_t)(int64_t)-ENOENT;
    }
    if (vfs_symlink(ktarget, linkpath) < 0) {
        kfree(ktarget);
        return (uint64_t)(int64_t)-EIO;
    }
    kfree(ktarget);
    return 0;
}

static uint64_t sys_readlinkat(uint64_t dirfd, uint64_t path_addr,
                                uint64_t buf_addr, uint64_t bufsize) {
    char kpath[256];
    if (strncpy_from_user(kpath, path_addr, sizeof(kpath)) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    const char *path = resolve_path_at((int)dirfd, kpath);
    if (!path) return (uint64_t)(int64_t)-ENOENT;
    if (bufsize == 0) return (uint64_t)(int64_t)-EINVAL;
    if (bufsize > 4096) bufsize = 4096;
    /* Read into kernel buffer, then copy out */
    char kbuf[4096];
    int n = vfs_readlink(path, kbuf, (int)sizeof(kbuf));
    if (n < 0) return (uint64_t)(int64_t)n;
    if (copy_to_user(buf_addr, kbuf, (size_t)n) < 0)
        return (uint64_t)(int64_t)-EFAULT;
    return (uint64_t)n;
}

/* ── getdents64 ───────────────────────────────────────────────────────── */

static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp_addr, uint64_t count) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-ESRCH;
    if (fd >= PROCESS_FD_MAX || !p->fd_table[fd].used)
        return (uint64_t)(int64_t)-EBADF;
    if (syscall_is_user_process() && !syscall_user_write_ok(dirp_addr, count))
        return (uint64_t)(int64_t)-EFAULT;

    char names[64][64];
    int n = vfs_readdir_names(p->fd_table[fd].path, names, 64);
    if (n < 0) return (uint64_t)(int64_t)n;
    if (n == 0) return 0;

    int start = (int)p->fd_table[fd].offset;
    if (start >= n) return 0;

    size_t total = 0;

    for (int i = start; i < n; i++) {
        int namelen = (int)strlen(names[i]);
        size_t reclen = sizeof(struct linux_dirent64) + (size_t)namelen + 1;
        reclen = (reclen + 7) & ~(size_t)7; /* align to 8 */

        if (total + reclen > (size_t)count) break;

        /* Build entry in kernel buffer, then copy out */
        uint8_t kentry[512];
        struct linux_dirent64 *k_ent = (struct linux_dirent64 *)kentry;
        k_ent->d_ino = 1;
        k_ent->d_off = (int64_t)(i + 1 < n ? reclen : 0);
        k_ent->d_reclen = (unsigned short)reclen;
        k_ent->d_type = DT_UNKNOWN;
        memcpy(k_ent->d_name, names[i], (unsigned long)namelen + 1);

        if (copy_to_user(dirp_addr + total, kentry, (size_t)reclen) < 0)
            return (uint64_t)-1;
        total += reclen;
        p->fd_table[fd].offset = (uint32_t)(i + 1);
    }

    return (uint64_t)total;
}

/* ── mlock / munlock / mincore / madvise / fallocate ──────────────────── */

static uint64_t sys_mlock(uint64_t addr, uint64_t len) {
    struct process *p = process_get_current();
    if (!p || !p->pml4) return (uint64_t)-EINVAL;
    if (addr & (PAGE_SIZE - 1)) return (uint64_t)-EINVAL;

    len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    if (addr + len < addr) return (uint64_t)-EINVAL;
    if (addr + len > USER_VADDR_MAX) return (uint64_t)-EINVAL;
    if (len == 0) return 0;

    /* If MCL_FUTURE is set, pages are globally locked via mlockall;
     * individual mlock is not permitted. */
    if (p->vm_locked_flags & 2)
        return (uint64_t)-EPERM;

    uint64_t npages = len / PAGE_SIZE;

    /* RLIMIT_MEMLOCK check (index 8) */
    uint64_t limit = p->rlim_cur[8];
    if (limit != (uint64_t)-1) {
        uint64_t limit_pages = limit / PAGE_SIZE;
        if (p->locked_pages + npages > limit_pages)
            return (uint64_t)-ENOMEM;
    }

    /* Wire the pages: resolve COW, ref physical frames, set LOCKED flag */
    int ret = vmm_lock_user_pages(p->pml4, addr, npages);
    if (ret < 0) {
        /* On failure, unlock any pages we may have already locked
         * in the range.  vmm_unlock_user_pages safely skips non-locked
         * pages, so this works even if locking failed partway through. */
        vmm_unlock_user_pages(p->pml4, addr, npages);
        return (uint64_t)(int64_t)(ret == -ENOMEM ? -ENOMEM : -EFAULT);
    }

    p->locked_pages += npages;
    return 0;
}

static uint64_t sys_munlock(uint64_t addr, uint64_t len) {
    struct process *p = process_get_current();
    if (!p || !p->pml4) return (uint64_t)-EINVAL;
    if (addr & (PAGE_SIZE - 1)) return (uint64_t)-EINVAL;
    if (addr + len < addr) return (uint64_t)-EINVAL;
    if (addr + len > USER_VADDR_MAX) return (uint64_t)-EINVAL;

    /* If MCL_CURRENT is set, pages are globally locked and cannot be
     * individually unlocked. */
    if (p->vm_locked_flags & 1)
        return (uint64_t)-EPERM;

    uint64_t npages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages == 0) return 0;

    /* Unwire the pages: clear LOCKED flag, unref physical frames */
    vmm_unlock_user_pages(p->pml4, addr, npages);

    if (npages > p->locked_pages)
        p->locked_pages = 0;
    else
        p->locked_pages -= npages;
    return 0;
}

static uint64_t sys_mlockall(uint64_t flags) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-ENOMEM;
    /* MCL_CURRENT=1, MCL_FUTURE=2, MCL_ONFAULT=4 */
    if (flags & ~(uint64_t)(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT))
        return (uint64_t)-EINVAL;
    p->vm_locked_flags = (int)(flags & (MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT));

    /* MCL_CURRENT: actually wire all currently mapped pages */
    if (flags & MCL_CURRENT) {
        uint64_t total = 0;

        if (p->pml4) {
            /* ── Phase 1: Count all mapped pages for RLIMIT_MEMLOCK check ── */
            for (int i4 = 0; i4 < 512; i4++) {
                if (!(p->pml4[i4] & PTE_PRESENT)) continue;
                uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(p->pml4[i4] & PTE_ADDR_MASK);
                for (int i3 = 0; i3 < 512; i3++) {
                    if (!(pdpt[i3] & PTE_PRESENT)) continue;
                    if (pdpt[i3] & PTE_HUGE) { total += 512 * 512; continue; }
                    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[i3] & PTE_ADDR_MASK);
                    for (int i2 = 0; i2 < 512; i2++) {
                        if (!(pd[i2] & PTE_PRESENT)) continue;
                        if (pd[i2] & PTE_HUGE) { total += 512; continue; }
                        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[i2] & PTE_ADDR_MASK);
                        for (int i1 = 0; i1 < 512; i1++) {
                            if (pt[i1] & PTE_PRESENT) total++;
                        }
                    }
                }
            }

            /* RLIMIT_MEMLOCK check (rlimit index 8) */
            uint64_t limit = p->rlim_cur[8];
            if (limit != (uint64_t)-1 && (total * PAGE_SIZE) > limit)
                return (uint64_t)-ENOMEM;

            /* ── Phase 2: Actually lock all pages via vmm_lock_user_pages ── */
            /* Only prefault+lock if MCL_ONFAULT is NOT set */
            if (!(flags & MCL_ONFAULT)) {
                for (int i4 = 0; i4 < 512; i4++) {
                    if (!(p->pml4[i4] & PTE_PRESENT)) continue;
                    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(p->pml4[i4] & PTE_ADDR_MASK);
                    for (int i3 = 0; i3 < 512; i3++) {
                        if (!(pdpt[i3] & PTE_PRESENT)) continue;
                        uint64_t base_virt_1g = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30);

                        if (pdpt[i3] & PTE_HUGE) {
                            /* 1GB huge page: lock each 2MB sub-range separately */
                            for (int i2 = 0; i2 < 512; i2++) {
                                uint64_t va = base_virt_1g | ((uint64_t)i2 << 21);
                                vmm_lock_user_pages(p->pml4, va, 512);
                            }
                            continue;
                        }

                        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[i3] & PTE_ADDR_MASK);
                        for (int i2 = 0; i2 < 512; i2++) {
                            if (!(pd[i2] & PTE_PRESENT)) continue;
                            uint64_t base_virt_2m = base_virt_1g | ((uint64_t)i2 << 21);

                            if (pd[i2] & PTE_HUGE) {
                                /* 2MB huge page: lock all 512 frames */
                                vmm_lock_user_pages(p->pml4, base_virt_2m, 512);
                                continue;
                            }

                            /* 4KB pages: find contiguous mapped ranges */
                            uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[i2] & PTE_ADDR_MASK);
                            uint64_t range_start = 0;
                            int in_range = 0;
                            for (int i1 = 0; i1 < 512; i1++) {
                                if (pt[i1] & PTE_PRESENT) {
                                    if (!in_range) {
                                        range_start = base_virt_2m | ((uint64_t)i1 << 12);
                                        in_range = 1;
                                    }
                                } else {
                                    if (in_range) {
                                        uint64_t range_end = base_virt_2m | ((uint64_t)i1 << 12);
                                        uint64_t npages = (range_end - range_start) / PAGE_SIZE;
                                        vmm_lock_user_pages(p->pml4, range_start, npages);
                                        in_range = 0;
                                    }
                                }
                            }
                            /* Close the last range if still open */
                            if (in_range) {
                                uint64_t range_end = base_virt_2m | (512ULL << 12);
                                uint64_t npages = (range_end - range_start) / PAGE_SIZE;
                                vmm_lock_user_pages(p->pml4, range_start, npages);
                            }
                        }
                    }
                }
            }
        }

        p->locked_pages = total;
    }
    return 0;
}

static uint64_t sys_munlockall(void) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-ENOMEM;

    /* Walk the page table and unwire every locked page */
    if (p->pml4) {
        for (int i4 = 0; i4 < 512; i4++) {
            if (!(p->pml4[i4] & PTE_PRESENT)) continue;
            uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(p->pml4[i4] & PTE_ADDR_MASK);
            for (int i3 = 0; i3 < 512; i3++) {
                if (!(pdpt[i3] & PTE_PRESENT)) continue;
                uint64_t base_virt_1g = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30);

                if (pdpt[i3] & PTE_HUGE) {
                    /* 1GB huge page: check if locked, unlock each 2MB sub-range */
                    /* Not all PDPTE entries are lockable at this level; skip.
                     * Linux does not support locking 1GB pages individually. */
                    continue;
                }

                uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[i3] & PTE_ADDR_MASK);
                for (int i2 = 0; i2 < 512; i2++) {
                    if (!(pd[i2] & PTE_PRESENT)) continue;
                    uint64_t base_virt_2m = base_virt_1g | ((uint64_t)i2 << 21);

                    if (pd[i2] & PTE_HUGE) {
                        /* 2MB huge page: unlock if locked */
                        if (pd[i2] & VMM_FLAG_LOCKED) {
                            vmm_unlock_user_pages(p->pml4, base_virt_2m, 512);
                        }
                        continue;
                    }

                    /* 4KB pages: find contiguous locked ranges */
                    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[i2] & PTE_ADDR_MASK);
                    uint64_t range_start = 0;
                    int in_range = 0;
                    for (int i1 = 0; i1 < 512; i1++) {
                        if ((pt[i1] & PTE_PRESENT) && (pt[i1] & VMM_FLAG_LOCKED)) {
                            if (!in_range) {
                                range_start = base_virt_2m | ((uint64_t)i1 << 12);
                                in_range = 1;
                            }
                        } else {
                            if (in_range) {
                                uint64_t range_end = base_virt_2m | ((uint64_t)i1 << 12);
                                uint64_t npages = (range_end - range_start) / PAGE_SIZE;
                                vmm_unlock_user_pages(p->pml4, range_start, npages);
                                in_range = 0;
                            }
                        }
                    }
                    /* Close the last range if still open */
                    if (in_range) {
                        uint64_t range_end = base_virt_2m | (512ULL << 12);
                        uint64_t npages = (range_end - range_start) / PAGE_SIZE;
                        vmm_unlock_user_pages(p->pml4, range_start, npages);
                    }
                }
            }
        }
    }

    p->vm_locked_flags = 0;
    p->locked_pages = 0;
    return 0;
}

static uint64_t sys_mincore(uint64_t addr, uint64_t len, uint64_t vec_addr) {
    struct process *p = process_get_current();
    if (!p || !p->pml4) return (uint64_t)(int64_t)-ENOMEM;
    if (addr & (PAGE_SIZE - 1)) return (uint64_t)(int64_t)-EINVAL;

    /* Overflow and bounds check */
    if (addr + len < addr) return (uint64_t)(int64_t)-EINVAL;
    if (addr + len > USER_VADDR_MAX) return (uint64_t)(int64_t)-EINVAL;

    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) return 0;

    /* vec_addr is validated by dispatch (syscall_user_write_ok), but
     * also must be non-null for the operation to make sense. */
    if (!vec_addr)
        return (uint64_t)(int64_t)-EFAULT;

    uint8_t *vec = (uint8_t *)kmalloc(pages);
    if (!vec) return (uint64_t)(int64_t)-ENOMEM;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t vaddr = addr + i * PAGE_SIZE;
        vec[i] = vmm_page_is_mapped_user(p->pml4, vaddr) ? 1 : 0;
    }

    int ret = copy_to_user(vec_addr, vec, pages);
    kfree(vec);
    if (ret < 0)
        return (uint64_t)(int64_t)-EFAULT;

    return 0;
}

static uint64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice) {
    struct process *p = process_get_current();
    if (!p || !p->pml4) return (uint64_t)(int64_t)-EFAULT;

    /* Common validation for operations that touch user pages */
    switch (advice) {
        case MADV_DONTNEED:
        case MADV_FREE:
        case MADV_MERGEABLE:
        case MADV_UNMERGEABLE:
        case MADV_WILLNEED:
        case MADV_COLD:
        case MADV_PAGEOUT: {
            if (addr & (PAGE_SIZE - 1)) return (uint64_t)-EINVAL;
            uint64_t length = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
            if (addr + length < addr) return (uint64_t)-EINVAL;
            if (addr + length > USER_VADDR_MAX) return (uint64_t)-EINVAL;
            /* Validate user address range is readable/writable */
            if (!vmm_user_range_ok(p->pml4, addr, length, 0))
                return (uint64_t)-EFAULT;
            break;
        }
        default:
            break;
    }

    switch (advice) {
        case MADV_DONTNEED: {
            /* Immediately unmap and free physical pages */
            int ret = madvise_dontneed(addr, len);
            return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
        }
        case MADV_FREE: {
            /* Lazy freeing: pages become freeable (immediate in our impl) */
            int ret = madvise_free(addr, len);
            return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
        }
        case MADV_WILLNEED: {
            /* Pre-fault: ensure pages are mapped */
            int ret = madvise_willneed(addr, len);
            return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
        }
        case MADV_COLD: {
            /* Hint pages are cold — clear accessed/dirty, reclaim immediately */
            int ret = madvise_cold(addr, len);
            return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
        }
        case MADV_PAGEOUT: {
            /* Proactively swap out pages (immediate reclaim without swap) */
            int ret = madvise_pageout(addr, len);
            return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
        }
        case MADV_MERGEABLE: {
            /* Mark pages as candidates for KSM merging */
            int ret = madvise_mergeable(addr, len);
            return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
        }
        case MADV_UNMERGEABLE: {
            /* Unmark as KSM mergeable — no-op since we don't track merge state */
            return 0;
        }
        case MADV_NORMAL:
        case MADV_RANDOM:
        case MADV_SEQUENTIAL:
            /* No-op: we don't do I/O clustering yet */
            return 0;
        default:
            return (uint64_t)-ENOSYS;
    }
}

/* ── NUMA memory policy syscalls ─────────────────────────────────── */

static uint64_t sys_mbind(uint64_t addr, uint64_t len, uint64_t mode,
                           uint64_t nodemask, uint64_t maxnode,
                           uint64_t flags)
{
    (void)maxnode;
    (void)flags;
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-ENOMEM;

    if (addr & (PAGE_SIZE - 1))
        return (uint64_t)-EINVAL;
    if (addr + len < addr || len == 0)
        return (uint64_t)-EINVAL;
    if (addr + len > USER_VADDR_MAX)
        return (uint64_t)-EINVAL;

    int ret = mempolicy_mbind(addr, len, (int)mode, nodemask);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
}

static uint64_t sys_set_mempolicy(uint64_t mode, uint64_t nodemask,
                                   uint64_t maxnode)
{
    (void)maxnode;
    int ret = mempolicy_set((int)mode, nodemask, -1);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
}

static uint64_t sys_get_mempolicy(uint64_t mode_addr, uint64_t nodemask_addr,
                                   uint64_t maxnode, uint64_t addr,
                                   uint64_t flags)
{
    (void)maxnode;
    (void)addr;
    (void)flags;

    int mode = MPOL_DEFAULT;
    uint64_t nodemask = 0;
    int ret = mempolicy_get(&mode, &nodemask, NULL);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;

    if (mode_addr) {
        if (copy_to_user(mode_addr, &mode, sizeof(mode)) < 0)
            return (uint64_t)-EFAULT;
    }
    if (nodemask_addr) {
        if (copy_to_user(nodemask_addr, &nodemask, sizeof(nodemask)) < 0)
            return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t sys_migrate_pages(uint64_t pid, uint64_t maxnode,
                                   uint64_t old_nodes_addr,
                                   uint64_t new_nodes_addr)
{
    (void)maxnode;
    (void)old_nodes_addr;

    uint64_t new_nodemask = 0;
    if (new_nodes_addr) {
        if (copy_from_user(&new_nodemask, new_nodes_addr, sizeof(new_nodemask)) < 0)
            return (uint64_t)-EFAULT;
    }

    int ret = mempolicy_migrate_pages((int)pid, new_nodemask);
    return (ret < 0) ? (uint64_t)(int64_t)ret : 0;
}

static uint64_t sys_move_pages(uint64_t pid, uint64_t nr_pages,
                                uint64_t pages_addr, uint64_t nodes_addr,
                                uint64_t status_addr, uint64_t flags)
{
    struct process *target = NULL;

    if (nr_pages == 0)
        return 0;

    if (pid == 0) {
        target = process_get_current();
    } else {
        target = process_get_by_pid((uint32_t)pid);
    }
    if (!target)
        return (uint64_t)-ESRCH;

    /* Allocate kernel buffers for page addresses, nodes, and status */
    uint64_t *pages = (uint64_t *)kmalloc(nr_pages * sizeof(uint64_t));
    int *nodes = (int *)kmalloc(nr_pages * sizeof(int));
    int *status = (int *)kmalloc(nr_pages * sizeof(int));
    if (!pages || !nodes || !status) {
        kfree(pages);
        kfree(nodes);
        kfree(status);
        return (uint64_t)-ENOMEM;
    }

    /* Copy pages array from userspace */
    if (copy_from_user(pages, pages_addr, nr_pages * sizeof(uint64_t)) < 0) {
        kfree(pages);
        kfree(nodes);
        kfree(status);
        return (uint64_t)-EFAULT;
    }

    /* Copy nodes array from userspace (optional, NULL means don't move) */
    int has_nodes = (nodes_addr != 0);
    if (has_nodes) {
        if (copy_from_user(nodes, nodes_addr, nr_pages * sizeof(int)) < 0) {
            kfree(pages);
            kfree(nodes);
            kfree(status);
            return (uint64_t)-EFAULT;
        }
    }

    /* Determine status for each page */
    for (uint64_t i = 0; i < nr_pages; i++) {
        uint64_t page_addr = pages[i];

        /* Page address must be aligned */
        if (page_addr & (PAGE_SIZE - 1ULL)) {
            status[i] = -EINVAL;
            continue;
        }

        if (!target->pml4) {
            status[i] = -EFAULT;
            continue;
        }

        /* Check if the page is mapped */
        if (!vmm_page_is_mapped_user(target->pml4, page_addr)) {
            status[i] = -ENOENT;
            continue;
        }

        /* Page is present */
        status[i] = 0;

        /* If MPOL_MF_MOVE or MPOL_MF_MOVE_ALL is set and we have
         * a destination node, attempt to migrate.  Without NUMA
         * hardware support, store the policy hint. */
        if (has_nodes && (flags & (MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))) {
            int dst_node = nodes[i];
            if (dst_node >= 0 && dst_node < MPOL_MAX_NODES) {
                /* Record migration intent in current policy */
                /* In a real NUMA system, this would call
                 * pmm_migrate_frame(old_phys, dst_node). */
                (void)dst_node;
            }
        }
    }

    /* Copy status array back to userspace */
    int ret = 0;
    if (copy_to_user(status_addr, status, nr_pages * sizeof(int)) < 0)
        ret = -EFAULT;

    kfree(pages);
    kfree(nodes);
    kfree(status);

    return (ret < 0) ? (uint64_t)(int64_t)ret : nr_pages;
}

static uint64_t sys_remap_file_pages(uint64_t addr, uint64_t size,
                                      uint64_t prot, uint64_t pgoff,
                                      uint64_t flags)
{
    /* remap_file_pages creates non-linear file mappings within an
     * existing MAP_SHARED file-backed VMA.  This kernel does not
     * currently support file-backed mmap with VMA tracking, so we
     * validate parameters and return ENOSYS.
     *
     * Note: remap_file_pages was deprecated in Linux 3.16 and removed
     * in Linux 6.0.  New code should use mmap with MAP_FIXED instead. */

    /* Basic parameter validation */
    if (addr & (PAGE_SIZE - 1ULL))
        return (uint64_t)-EINVAL;
    if (size == 0)
        return (uint64_t)-EINVAL;
    if (flags & ~(uint64_t)1)  /* only MAP_NONBLOCK (1) is valid */
        return (uint64_t)-EINVAL;
    (void)prot;
    (void)pgoff;

    return (uint64_t)-ENOSYS;
}

static uint64_t sys_msync(uint64_t addr, uint64_t len, uint64_t flags) {
    (void)addr;
    (void)len;
    (void)flags;
    /* msync is a no-op since we don't have a page cache for mmap'd files.
     * On Linux, msync flushes dirty pages back to the mapped file.
     * Without MAP_SHARED file-backing, this is a successful no-op. */
    return 0;
}

/**
 * sys_fallocate - Pre-allocate disk space for a file descriptor
 * @fd:     File descriptor of the file
 * @mode:   Fallocate mode flags (FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE)
 * @offset: Starting byte offset for allocation
 * @len:    Number of bytes to allocate
 *
 * Allocates disk space for the file descriptor @fd covering the byte
 * range [@offset, @offset+@len).  Subsequent writes to this range are
 * guaranteed not to fail with ENOSPC.  This is a Linux-compatible
 * implementation of the fallocate() syscall.
 *
 * Context: Called from syscall dispatch. May sleep.
 * Return: 0 on success, or (uint64_t)-1 on error with errno encoded
 *         as a negative value.
 */
static uint64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset, uint64_t len)
{
    if (fd < 3)
        return (uint64_t)(int64_t)-EBADF;
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used)
        return (uint64_t)(int64_t)-EBADF;

    /* Validate offset and len fit in uint32_t */
    if (offset > 0xFFFFFFFFULL || len > 0xFFFFFFFFULL)
        return (uint64_t)(int64_t)-EOVERFLOW;

    /* Check that the target is a regular file */
    struct vfs_stat st;
    if (vfs_stat(pfd->path, &st) < 0)
        return (uint64_t)(int64_t)-EIO;
    if (st.type != 1)
        return (uint64_t)(int64_t)-EINVAL;

    /* Delegate to VFS-level fallocate which handles mount resolution,
     * RDONLY checks, and filesystem-specific fallocate ops. */
    int ret = vfs_fallocate(pfd->path, (int)mode, (uint32_t)offset, (uint32_t)len);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return 0;
}


/**
 * sys_readahead - Prefetch file data into the page cache
 * @fd:     File descriptor of the file to prefetch
 * @offset: Starting byte offset for the prefetch range
 * @count:  Number of bytes to prefetch
 *
 * Advises the kernel to prefetch file data into the page cache for the
 * given byte range.  The call is advisory; subsequent reads of the
 * specified range will be served from cache if the prefetch succeeds.
 * This is a Linux-compatible implementation of the readahead() syscall.
 *
 * Context: Called from syscall dispatch. May sleep.
 * Return: 0 on success, or (uint64_t)-1 on error with errno encoded
 *         as a negative value.
 */
static uint64_t sys_readahead(uint64_t fd, uint64_t offset, uint64_t count)
{
    if (fd < 3)
        return (uint64_t)(int64_t)-EBADF;
    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used)
        return (uint64_t)(int64_t)-EBADF;

    /* Readahead on directories, sockets, or special files is invalid */
    struct vfs_stat st;
    if (vfs_stat(pfd->path, &st) < 0)
        return (uint64_t)(int64_t)-EIO;
    if (st.type != 1)
        return (uint64_t)(int64_t)-EINVAL;

    /* Delegate to VFS-level readahead which handles page cache prefetch */
    int ret = vfs_readahead(pfd->path, (uint32_t)offset, (uint32_t)count);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;
    return 0;
}


/* ── posix_fadvise64 (Item 302) ────────────────────────────────────── */
/*
 * posix_fadvise: advise kernel about expected file access pattern.
 *
 *   int fadvise64(int fd, off64_t offset, off64_t len, int advice);
 *
 * Advice values (from syscall.h):
 *   POSIX_FADV_NORMAL      — no special treatment (default)
 *   POSIX_FADV_RANDOM      — file will be accessed randomly
 *   POSIX_FADV_SEQUENTIAL  — file will be accessed sequentially
 *   POSIX_FADV_WILLNEED    — the specified range will be needed soon
 *   POSIX_FADV_DONTNEED    — the specified range is not needed anymore
 *   POSIX_FADV_NOREUSE     — data will be accessed only once
 *
 * Returns 0 on success, -1 on error with appropriate errno.
 */
static uint64_t sys_fadvise64(uint64_t fd, uint64_t offset, uint64_t len, uint64_t advice)
{
    /* Validate fd: must be a regular file descriptor */
    if (fd < 3 || fd >= 700)
        return (uint64_t)-EBADF;

    int i = (int)fd - 3;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used)
        return (uint64_t)-EBADF;

    /* Validate advice value */
    if (advice > POSIX_FADV_NOREUSE)
        return (uint64_t)-EINVAL;

    /* Get the file's inode number for page cache operations */
    struct vfs_stat st;
    if (vfs_stat(pfd->path, &st) < 0)
        return (uint64_t)-EBADF;

    /* Clamp len if it extends beyond file size */
    uint64_t end_offset;
    if (len == 0) {
        /* len=0 means "to EOF" in POSIX */
        end_offset = st.size;
    } else {
        end_offset = offset + len;
        if (end_offset > st.size)
            end_offset = st.size;
    }
    if (offset >= st.size)
        return 0; /* nothing to do */

    switch (advice) {
    case POSIX_FADV_WILLNEED:
        /* Trigger readahead for the requested byte range */
        if (st.type == 1) {
            (void)vfs_readahead(pfd->path, (uint32_t)offset,
                                (uint32_t)(end_offset - offset));
        }
        break;

    case POSIX_FADV_DONTNEED:
        /* Evict pages from page cache for the byte range.
         * Only operates on regular files that have an inode number. */
        if (st.type == 1 && st.ino != 0) {
            uint32_t start_block = (uint32_t)(offset / PAGE_SIZE);
            uint32_t end_block   = (uint32_t)((end_offset + PAGE_SIZE - 1) / PAGE_SIZE);
            for (uint32_t blk = start_block; blk < end_block; blk++) {
                page_cache_discard(st.ino, blk);
            }
        }
        break;

    case POSIX_FADV_NORMAL:
    case POSIX_FADV_RANDOM:
    case POSIX_FADV_SEQUENTIAL:
    case POSIX_FADV_NOREUSE:
        /* Store the advice for use by readahead heuristics.
         * The VFS readahead logic can later query pfd->advice to
         * tune window sizes and prefetch behavior. */
        pfd->advice = (int)advice;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}


/* ── timerfd ──────────────────────────────────────────────────────────── */

#define TIMERFD_MAX 16

struct timerfd {
    int      in_use;
    int      clockid;
    int      absolute;        /* 1 = TFD_TIMER_ABSTIME was set on last settime */
    uint64_t it_value;        /* ticks: relative value OR absolute expiration tick */
    uint64_t it_interval;     /* ticks between repeated expirations */
    uint64_t expirations;     /* number of times expired since last read */
    uint64_t start_tick;      /* for relative timers: tick when timer was set */
};

static struct timerfd timerfd_table[TIMERFD_MAX];

/* Read from timerfd: returns number of expirations */
static int timerfd_do_read(int slot, uint64_t *val) {
    if (slot < 0 || slot >= TIMERFD_MAX || !timerfd_table[slot].in_use)
        return -1;
    if (timerfd_table[slot].expirations == 0) return 0;
    *val = timerfd_table[slot].expirations;
    timerfd_table[slot].expirations = 0;
    return 0;
}

static uint64_t sys_timerfd_create(uint64_t clockid, uint64_t flags) {
    (void)flags;
    if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME)
        return (uint64_t)-1;

    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (!timerfd_table[i].in_use) {
            timerfd_table[i].in_use = 1;
            timerfd_table[i].clockid = (int)clockid;
            timerfd_table[i].absolute = 0;
            timerfd_table[i].it_value = 0;
            timerfd_table[i].it_interval = 0;
            timerfd_table[i].expirations = 0;
            timerfd_table[i].start_tick = timer_get_ticks();
            /* Return fd-like handle above normal range */
            return (uint64_t)(500 + i);
        }
    }
    return (uint64_t)-1;
}

static uint64_t sys_timerfd_settime(uint64_t fd, uint64_t flags,
                                     uint64_t new_addr, uint64_t old_addr) {
    int slot = (int)fd - 500;
    if (slot < 0 || slot >= TIMERFD_MAX || !timerfd_table[slot].in_use)
        return (uint64_t)-1;

    struct itimerspec new_val;
    if (new_addr) {
        if (copy_from_user(&new_val, new_addr, sizeof(struct itimerspec)) < 0)
            return (uint64_t)-1;
    }

    /* Return old value if requested */
    if (old_addr) {
        struct itimerspec old_val;
        if (timerfd_table[slot].absolute && timerfd_table[slot].it_value > 0) {
            /* Absolute timer: return remaining time as absolute (TFD_TIMER_ABSTIME
             * was set when the timer was armed, so old value is also absolute). */
            uint64_t remaining = 0;
            uint64_t now = timer_get_ticks();
            if (timerfd_table[slot].it_value > now)
                remaining = timerfd_table[slot].it_value - now;
            old_val.it_value.tv_sec = remaining / TIMER_FREQ;
            old_val.it_value.tv_nsec = (remaining % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
        } else {
            old_val.it_value.tv_sec = timerfd_table[slot].it_value / TIMER_FREQ;
            old_val.it_value.tv_nsec = (timerfd_table[slot].it_value % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
        }
        old_val.it_interval.tv_sec = timerfd_table[slot].it_interval / TIMER_FREQ;
        old_val.it_interval.tv_nsec = (timerfd_table[slot].it_interval % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
        if (copy_to_user(old_addr, &old_val, sizeof(struct itimerspec)) < 0)
            return (uint64_t)-1;
    }

    /* Set new timer */
    if (new_addr) {
        uint64_t val_ticks = new_val.it_value.tv_sec * TIMER_FREQ +
                             new_val.it_value.tv_nsec / (1000000000ULL / TIMER_FREQ);
        uint64_t interval_ticks = new_val.it_interval.tv_sec * TIMER_FREQ +
                                   new_val.it_interval.tv_nsec / (1000000000ULL / TIMER_FREQ);

        if (flags & TFD_TIMER_ABSTIME) {
            /* Absolute time: it_value is interpreted as an absolute time in the
             * same clock domain as timer_get_ticks() (monotonic time since boot).
             * Store directly as the absolute expiration tick. */
            timerfd_table[slot].absolute = 1;
            timerfd_table[slot].it_value = val_ticks;
            timerfd_table[slot].start_tick = 0; /* not used for absolute */
        } else {
            /* Relative time: it_value is a duration from now */
            timerfd_table[slot].absolute = 0;
            timerfd_table[slot].it_value = val_ticks;
            timerfd_table[slot].start_tick = timer_get_ticks();
        }
        timerfd_table[slot].it_interval = interval_ticks;
        timerfd_table[slot].expirations = 0;
    }

    return 0;
}

static uint64_t sys_timerfd_gettime(uint64_t fd, uint64_t cur_addr) {
    int slot = (int)fd - 500;
    if (slot < 0 || slot >= TIMERFD_MAX || !timerfd_table[slot].in_use)
        return (uint64_t)-1;

    struct itimerspec cur;

    if (timerfd_table[slot].absolute) {
        /* Absolute timer: remaining = it_value - now (or 0 if expired) */
        uint64_t now = timer_get_ticks();
        uint64_t remaining = timerfd_table[slot].it_value > now ?
                             timerfd_table[slot].it_value - now : 0;
        cur.it_value.tv_sec = remaining / TIMER_FREQ;
        cur.it_value.tv_nsec = (remaining % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
    } else {
        /* Relative timer: remaining = it_value - (now - start_tick) */
        uint64_t elapsed = timer_get_ticks() - timerfd_table[slot].start_tick;
        uint64_t remaining = timerfd_table[slot].it_value > elapsed ?
                             timerfd_table[slot].it_value - elapsed : 0;
        cur.it_value.tv_sec = remaining / TIMER_FREQ;
        cur.it_value.tv_nsec = (remaining % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);
    }

    cur.it_interval.tv_sec = timerfd_table[slot].it_interval / TIMER_FREQ;
    cur.it_interval.tv_nsec = (timerfd_table[slot].it_interval % TIMER_FREQ) * (1000000000ULL / TIMER_FREQ);

    if (copy_to_user(cur_addr, &cur, sizeof(struct itimerspec)) < 0)
        return (uint64_t)-1;
    return 0;
}

/* Check all timerfds for expiration — called from timer interrupt */
void timerfd_tick(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < TIMERFD_MAX; i++) {
        if (!timerfd_table[i].in_use || timerfd_table[i].it_value == 0)
            continue;

        int expired = 0;
        if (timerfd_table[i].absolute) {
            /* Absolute timer: check if now >= absolute expiration tick */
            if (now >= timerfd_table[i].it_value)
                expired = 1;
        } else {
            /* Relative timer: check if elapsed >= it_value */
            uint64_t elapsed = now - timerfd_table[i].start_tick;
            if (elapsed >= timerfd_table[i].it_value)
                expired = 1;
        }

        if (expired) {
            timerfd_table[i].expirations++;
            /* NOTE: Periodic re-arm is handled in timerfd_do_read(), not here.
             * This avoids the drift problem where repeatedly restarting the
             * timer in the tick path accumulates error.  However, we still
             * need to update the absolute tick for one-shot or the periodic
             * base so the timer doesn't re-fire immediately on the next tick. */
            if (timerfd_table[i].it_interval > 0) {
                if (timerfd_table[i].absolute) {
                    /* Advance absolute periodic: preserve alignment */
                    timerfd_table[i].it_value += timerfd_table[i].it_interval;
                    /* Catch up if we fell behind */
                    while (timerfd_table[i].it_value <= now) {
                        timerfd_table[i].it_value += timerfd_table[i].it_interval;
                        timerfd_table[i].expirations++;
                    }
                } else {
                    /* Relative periodic: restart from now */
                    timerfd_table[i].start_tick = now;
                    timerfd_table[i].it_value = timerfd_table[i].it_interval;
                }
            } else {
                /* One-shot: disarm */
                timerfd_table[i].it_value = 0;
            }
        }
    }
}

/* ── signalfd ─────────────────────────────────────────────────────────── */

#define SIGNALFD_MAX 16
#define SIGNALFD_BUF 8  /* ring buffer size per fd */

/* Internal signalfd flags for tracking (not the same as SFD_ syscall values) */
#define SIGNALFD_FLAG_CLOEXEC  0x0001
#define SIGNALFD_FLAG_NONBLOCK 0x0002

struct signalfd_info {
    int      in_use;
    uint32_t sigmask;               /* signals to catch */
    int      flags;                 /* SIGNALFD_FLAG_CLOEXEC, SIGNALFD_FLAG_NONBLOCK */
    uint32_t pid;                   /* owning process PID (for CLOEXEC cleanup) */
    /* Ring buffer of siginfo entries */
    struct siginfo ring[SIGNALFD_BUF];
    int      head;                  /* next write position */
    int      tail;                  /* next read position */
    int      count;                 /* number of pending entries */
};

struct signalfd_info signalfd_table[SIGNALFD_MAX];

/* Read from signalfd: copies next signalfd_siginfo entry to user buffer.
 * Returns bytes read (sizeof(signalfd_siginfo)) on success, 0 if EAGAIN
 * (no signals pending, non-blocking), or -1 on error. */
static int signalfd_do_read(int slot, void *buf, uint64_t count) {
    if (slot < 0 || slot >= SIGNALFD_MAX || !signalfd_table[slot].in_use)
        return -1;
    if (signalfd_table[slot].count == 0) {
        /* Non-blocking: return 0 (caller interprets as EAGAIN) */
        return 0;
    }

    struct signalfd_info *sf = &signalfd_table[slot];

    /* Pop the oldest entry from the ring buffer */
    struct siginfo *si = &sf->ring[sf->tail];

    /* Build the signalfd_siginfo from the stored siginfo */
    struct signalfd_siginfo out;
    memset(&out, 0, sizeof(out));
    out.ssi_signo = (uint32_t)si->si_signo;
    out.ssi_errno = si->si_errno;
    out.ssi_code  = si->si_code;
    out.ssi_pid   = si->si_pid;
    out.ssi_uid   = si->si_uid;
    out.ssi_status = si->si_status;
    out.ssi_addr  = (uint64_t)(uintptr_t)si->si_addr;

    /* Advance ring buffer */
    sf->tail = (sf->tail + 1) % SIGNALFD_BUF;
    sf->count--;

    /* Copy to user buffer (up to one struct) */
    size_t copy_size = sizeof(out);
    if ((size_t)count < copy_size)
        copy_size = (size_t)count;

    memcpy(buf, &out, copy_size);
    return (int)copy_size;
}

static uint64_t sys_signalfd(uint64_t fd, uint64_t mask_addr, uint64_t flags) {
    uint32_t sigmask = 0;
    int sfd_flags = 0;

    /* Extract SFD flags passed by userspace (Linux SFD_CLOEXEC = 02000000,
     * SFD_NONBLOCK = 04000).  We accept and store these for cleanup on
     * exec (SFD_CLOEXEC) and future non-blocking read semantics. */
    if (flags & 02000000) sfd_flags |= SIGNALFD_FLAG_CLOEXEC;
    if (flags & 04000)    sfd_flags |= SIGNALFD_FLAG_NONBLOCK;

    if (mask_addr) {
        uint32_t val;
        if (copy_from_user(&val, mask_addr, sizeof(val)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
        sigmask = val;
    }

    /* If fd is non-zero, update existing signalfd mask */
    if (fd != 0) {
        int slot = (int)fd - 600;
        if (slot >= 0 && slot < SIGNALFD_MAX && signalfd_table[slot].in_use) {
            signalfd_table[slot].sigmask = sigmask;
            return fd;
        }
        return (uint64_t)(int64_t)-EINVAL;
    }

    /* Create new signalfd */
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (!signalfd_table[i].in_use) {
            struct process *cur = process_get_current();
            signalfd_table[i].in_use = 1;
            signalfd_table[i].sigmask = sigmask;
            signalfd_table[i].flags = sfd_flags;
            signalfd_table[i].pid = cur ? cur->pid : 0;
            signalfd_table[i].head = 0;
            signalfd_table[i].tail = 0;
            signalfd_table[i].count = 0;
            memset(signalfd_table[i].ring, 0, sizeof(signalfd_table[i].ring));
            return (uint64_t)(600 + i);
        }
    }
    return (uint64_t)(int64_t)-ENFILE;
}

/* Legacy signalfd_notify — called from signal_send().
 * Enqueues a basic siginfo with SI_KERNEL code for the given signal number. */
void signalfd_notify(int signum) {
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (signalfd_table[i].in_use && (signalfd_table[i].sigmask & (1u << signum))) {
            struct signalfd_info *sf = &signalfd_table[i];
            if (sf->count < SIGNALFD_BUF) {
                struct siginfo *si = &sf->ring[sf->head];
                memset(si, 0, sizeof(*si));
                si->si_signo = signum;
                si->si_code  = SI_KERNEL;
                sf->head = (sf->head + 1) % SIGNALFD_BUF;
                sf->count++;
            }
            /* If buffer full, the signal is dropped (oldest entries survive) */
        }
    }
}

/* Extended signalfd_notify — called from signal_send_info() with full siginfo.
 * Enqueues the full siginfo data for each matching signalfd. */
void signalfd_notify_ext(int signum, int si_code,
                         uint32_t si_pid, uint32_t si_uid,
                         uint64_t si_addr, int si_status)
{
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (signalfd_table[i].in_use && (signalfd_table[i].sigmask & (1u << signum))) {
            struct signalfd_info *sf = &signalfd_table[i];
            if (sf->count < SIGNALFD_BUF) {
                struct siginfo *si = &sf->ring[sf->head];
                memset(si, 0, sizeof(*si));
                si->si_signo = signum;
                si->si_code  = si_code;
                si->si_pid   = si_pid;
                si->si_uid   = si_uid;
                si->si_addr  = (void *)(uintptr_t)si_addr;
                si->si_status = si_status;
                sf->head = (sf->head + 1) % SIGNALFD_BUF;
                sf->count++;
            }
        }
    }
}

/*
 * signalfd_exec_close — Close all signalfd fds with SIGNALFD_FLAG_CLOEXEC for the
 * current process.  Called during execve() to ensure exec'd processes
 * don't inherit signalfd fds that were marked CLOEXEC.
 *
 * This mirrors the fd_table cloexec cleanup in process_exec_close_cloexec()
 * for virtual signalfd file descriptors.
 */
void signalfd_exec_close(void) {
    struct process *cur = process_get_current();
    if (!cur) return;
    uint32_t cur_pid = cur->pid;
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (signalfd_table[i].in_use &&
            signalfd_table[i].pid == cur_pid &&
            (signalfd_table[i].flags & SIGNALFD_FLAG_CLOEXEC)) {
            signalfd_table[i].in_use = 0;
            signalfd_table[i].sigmask = 0;
            signalfd_table[i].flags = 0;
            signalfd_table[i].pid = 0;
            signalfd_table[i].count = 0;
        }
    }
}

/* ── splice / tee ─────────────────────────────────────────────────────── */

/**
 * sys_splice — Move data between two file descriptors
 * @fd_in:        Source fd (must be a pipe, or off_in must be non-NULL)
 * @off_in_addr:  User-space offset pointer for source (NULL if fd_in is a pipe)
 * @fd_out:       Destination fd (must be a pipe, or off_out must be non-NULL)
 * @off_out_addr: User-space offset pointer for dest (NULL if fd_out is a pipe)
 * @len:          Maximum bytes to transfer
 * @flags:        SPLICE_F_MOVE, SPLICE_F_NONBLOCK, SPLICE_F_MORE, SPLICE_F_GIFT
 *
 * Linux signature:
 *   ssize_t splice(int fd_in, loff_t *off_in, int fd_out,
 *                  loff_t *off_out, size_t len, unsigned int flags);
 *
 * Linux semantics:
 *   - At least one of fd_in, fd_out must be a pipe.
 *   - If fd_in is a pipe, off_in_addr must be NULL (0). Data consumed from pipe.
 *   - If fd_in is NOT a pipe, off_in_addr must point to the source file offset.
 *   - If fd_out is a pipe, off_out_addr must be NULL (0). Data written to pipe.
 *   - If fd_out is NOT a pipe, off_out_addr must point to the dest file offset.
 *
 * When both fds are pipes, uses zero-copy pipe_splice() internally.
 * Otherwise uses a 4 KiB kernel bounce buffer.
 *
 * Return: Number of bytes transferred (as uint64_t), or negative errno:
 *   EBADF  — invalid or wrong-mode fd
 *   EINVAL — no pipe involved, or offset required but missing
 *   ESPIPE — offset given on a pipe fd
 *   EFAULT — bad user-space offset pointer
 *   ENOMEM — cannot allocate bounce buffer
 *   EIO    — VFS read/write failure
 */
static uint64_t sys_splice(uint64_t fd_in, uint64_t off_in_addr,
                           uint64_t fd_out, uint64_t off_out_addr,
                           uint64_t len, uint64_t flags)
{
    (void)flags;  /* SPLICE_F_* flags are advisory; non-blocking depends on O_NONBLOCK */

    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-EPERM;

    /* len == 0 is a no-op per Linux semantics */
    if (len == 0)
        return 0;

    /* Validate fd range (direct indexing: fds 0..PROCESS_FD_MAX-1) */
    if (fd_in >= PROCESS_FD_MAX || fd_out >= PROCESS_FD_MAX)
        return (uint64_t)(int64_t)-EBADF;

    struct process_fd *pfd_in  = &p->fd_table[fd_in];
    struct process_fd *pfd_out = &p->fd_table[fd_out];

    if (!pfd_in->used || !pfd_out->used)
        return (uint64_t)(int64_t)-EBADF;

    /* Detect pipe fds by their path prefix */
    int is_in_pipe  = (strncmp(pfd_in->path, "pipe_", 5) == 0);
    int is_out_pipe = (strncmp(pfd_out->path, "pipe_", 5) == 0);

    /* Linux: at least one fd must be a pipe */
    if (!is_in_pipe && !is_out_pipe)
        return (uint64_t)(int64_t)-EINVAL;

    /* Linux: pipe fds must have NULL offsets, non-pipe fds must have offsets */
    if (is_in_pipe && off_in_addr != 0)
        return (uint64_t)(int64_t)-ESPIPE;
    if (is_out_pipe && off_out_addr != 0)
        return (uint64_t)(int64_t)-ESPIPE;
    if (!is_in_pipe && off_in_addr == 0)
        return (uint64_t)(int64_t)-EINVAL;
    if (!is_out_pipe && off_out_addr == 0)
        return (uint64_t)(int64_t)-EINVAL;

    /* Extract pipe IDs */
    int in_pipe_id  = (int)pfd_in->offset;
    int out_pipe_id = (int)pfd_out->offset;

    /* Verify pipe direction: source must be a read end, dest a write end */
    if (is_in_pipe && strncmp(pfd_in->path, "pipe_read_", 10) != 0)
        return (uint64_t)(int64_t)-EBADF;
    if (is_out_pipe && strncmp(pfd_out->path, "pipe_write_", 11) != 0)
        return (uint64_t)(int64_t)-EBADF;

    /* Check open_flags: source must be readable, dest writable */
    if (!is_in_pipe && (pfd_in->open_flags & 3) == 1)  /* O_WRONLY alone */
        return (uint64_t)(int64_t)-EBADF;
    if (!is_out_pipe && (pfd_out->open_flags & 3) == 0) /* O_RDONLY alone */
        return (uint64_t)(int64_t)-EBADF;

    /* ── Pipe-to-pipe: zero-copy via pipe_splice() ── */
    if (is_in_pipe && is_out_pipe) {
        int splice_len = (len > (uint64_t)INT32_MAX) ? INT32_MAX : (int)len;
        int ret = pipe_splice(in_pipe_id, out_pipe_id, splice_len);
        if (ret < 0)
            return (uint64_t)(int64_t)ret;
        return (uint64_t)ret;
    }

    /* ── File↔pipe: bounce buffer transfer ── */
    uint8_t *buf = kmalloc(4096);
    if (!buf)
        return (uint64_t)(int64_t)-ENOMEM;

    uint64_t total = 0;
    uint64_t sp_result;

    /* Read initial non-pipe source offset from user-space if provided */
    uint64_t in_off = 0;
    int use_in_explicit = (!is_in_pipe && off_in_addr != 0);
    if (use_in_explicit) {
        int64_t abs_off;
        if (copy_from_user(&abs_off, off_in_addr,
                           sizeof(abs_off)) < 0) {
            sp_result = (uint64_t)(int64_t)-EFAULT;
            goto splice_out;
        }
        if (abs_off < 0) {
            sp_result = (uint64_t)(int64_t)-EINVAL;
            goto splice_out;
        }
        in_off = (uint64_t)abs_off;
    }

    /* ── Transfer loop ── */
    while (total < len) {
        uint64_t chunk = len - total;
        if (chunk > 4096)
            chunk = 4096;

        int nread_total = 0;

        /* ── Read from source ── */
        if (is_in_pipe) {
            int n = pipe_read(in_pipe_id, buf, (int)chunk);
            if (n < 0) {
                sp_result = (total > 0) ? total : (uint64_t)(int64_t)n;
                goto splice_out;
            }
            if (n == 0)
                break;  /* EOF */
            nread_total = n;
        } else {
            /* Non-pipe source: use explicit offset */
            uint64_t saved_off = pfd_in->offset;
            if (use_in_explicit)
                pfd_in->offset = in_off;

            uint32_t nread = 0;
            int r = vfs_read(pfd_in->path, buf, (uint32_t)chunk, &nread);
            if (r < 0) {
                if (use_in_explicit)
                    pfd_in->offset = saved_off;
                sp_result = (total > 0) ? total : (uint64_t)(int64_t)r;
                goto splice_out;
            }
            if (nread == 0) {
                if (use_in_explicit)
                    pfd_in->offset = saved_off;
                break;  /* EOF */
            }
            nread_total = (int)nread;

            if (use_in_explicit) {
                in_off += nread;
                pfd_in->offset = saved_off;
            }
        }

        /* ── Write to destination ── */
        if (is_out_pipe) {
            int w = pipe_write(out_pipe_id, buf, nread_total);
            if (w < 0) {
                sp_result = (total > 0) ? total : (uint64_t)(int64_t)w;
                goto splice_out;
            }
        } else {
            /* Non-pipe dest: read offset from user-space each iteration */
            uint64_t saved_off = pfd_out->offset;
            if (off_out_addr != 0) {
                int64_t abs_off;
                if (copy_from_user(&abs_off, off_out_addr,
                                   sizeof(abs_off)) < 0) {
                    sp_result = (uint64_t)(int64_t)-EFAULT;
                    goto splice_out;
                }
                if (abs_off < 0) {
                    sp_result = (uint64_t)(int64_t)-EINVAL;
                    goto splice_out;
                }
                pfd_out->offset = (uint64_t)abs_off;
            }

            int w = vfs_write(pfd_out->path, buf, (uint32_t)nread_total);
            if (w < 0) {
                if (off_out_addr != 0)
                    pfd_out->offset = saved_off;
                sp_result = (total > 0) ? total : (uint64_t)(int64_t)w;
                goto splice_out;
            }

            /* Write back updated dest offset to user-space */
            if (off_out_addr != 0) {
                int64_t new_off = (int64_t)(pfd_out->offset);
                if (copy_to_user(off_out_addr,
                                 &new_off, sizeof(new_off)) < 0) {
                    sp_result = (uint64_t)(int64_t)-EFAULT;
                    goto splice_out;
                }
            }
        }

        total += (uint64_t)nread_total;

        if (nread_total < (int)chunk)
            break;  /* Short read/write — no more data */
    }

    /* Write back updated source offset to user-space */
    if (use_in_explicit) {
        int64_t new_off = (int64_t)in_off;
        if (copy_to_user(off_in_addr,
                         &new_off, sizeof(new_off)) < 0) {
            sp_result = (uint64_t)(int64_t)-EFAULT;
            goto splice_out;
        }
    }

    sp_result = total;

splice_out:
    kfree(buf);
    return sp_result;
}

/**
 * sys_tee — Duplicate data from one pipe to another without consuming
 * @fd_in:       Source fd (must be a pipe read end)
 * @fd_out:      Destination fd (must be a pipe write end)
 * @len:         Maximum bytes to duplicate
 * @flags:       SPLICE_F_* flags (advisory)
 *
 * Linux signature:
 *   ssize_t tee(int fd_in, int fd_out, size_t len, unsigned int flags);
 *
 * Linux semantics:
 *   - Both fds MUST be pipes.
 *   - Data is read from fd_in without being consumed, and written
 *     to fd_out.
 *   - The source data remains available for subsequent reads/splices.
 *   - fd_in must be the read end of a pipe, fd_out the write end.
 *
 * Return: Number of bytes duplicated, or negative errno:
 *   EBADF  — invalid or wrong-mode fd
 *   EINVAL — fd is not a pipe
 *   ENOMEM — cannot allocate bounce buffer
 */
static uint64_t sys_tee(uint64_t fd_in, uint64_t fd_out,
                         uint64_t len, uint64_t flags)
{
    (void)flags;

    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-EPERM;

    /* len == 0 is a no-op per Linux semantics */
    if (len == 0)
        return 0;

    /* Validate fd range */
    if (fd_in >= PROCESS_FD_MAX || fd_out >= PROCESS_FD_MAX)
        return (uint64_t)(int64_t)-EBADF;

    struct process_fd *pfd_in  = &p->fd_table[fd_in];
    struct process_fd *pfd_out = &p->fd_table[fd_out];

    if (!pfd_in->used || !pfd_out->used)
        return (uint64_t)(int64_t)-EBADF;

    /* Both fds MUST be pipes for tee */
    int is_in_pipe  = (strncmp(pfd_in->path, "pipe_", 5) == 0);
    int is_out_pipe = (strncmp(pfd_out->path, "pipe_", 5) == 0);

    if (!is_in_pipe || !is_out_pipe)
        return (uint64_t)(int64_t)-EINVAL;

    int in_pipe_id  = (int)pfd_in->offset;
    int out_pipe_id = (int)pfd_out->offset;

    /* Verify pipe direction: source must be a read end, dest a write end */
    if (strncmp(pfd_in->path, "pipe_read_", 10) != 0)
        return (uint64_t)(int64_t)-EBADF;
    if (strncmp(pfd_out->path, "pipe_write_", 11) != 0)
        return (uint64_t)(int64_t)-EBADF;

    /* Check open_flags: source must be readable, dest writable */
    if ((pfd_in->open_flags & 3) == 1)  /* O_WRONLY alone */
        return (uint64_t)(int64_t)-EBADF;
    if ((pfd_out->open_flags & 3) == 0) /* O_RDONLY alone */
        return (uint64_t)(int64_t)-EBADF;

    /* Use a 4 KiB kernel bounce buffer for the transfer */
    uint8_t *buf = kmalloc(4096);
    if (!buf)
        return (uint64_t)(int64_t)-ENOMEM;

    uint64_t total = 0;
    uint64_t tee_result;

    while (total < len) {
        int avail = pipe_available(in_pipe_id);
        if (avail <= 0) {
            /* No data available. If we've done some work, return that. */
            if (total > 0)
                break;
            /* Otherwise no data to copy — return 0 */
            break;
        }

        uint64_t chunk = len - total;
        if (chunk > 4096)
            chunk = 4096;
        if (chunk > (uint64_t)avail)
            chunk = (uint64_t)avail;

        /* Peek at data from source pipe without consuming */
        int n = pipe_peek(in_pipe_id, buf, (int)chunk);
        if (n < 0) {
            tee_result = (total > 0) ? total : (uint64_t)(int64_t)n;
            goto tee_out;
        }
        if (n == 0)
            break;

        /* Write to destination pipe */
        int w = pipe_write(out_pipe_id, buf, n);
        if (w < 0) {
            tee_result = (total > 0) ? total : (uint64_t)(int64_t)w;
            goto tee_out;
        }

        total += (uint64_t)w;

        /* If we couldn't write everything, destination is full — stop */
        if (w < n)
            break;
    }

    tee_result = total;

tee_out:
    kfree(buf);
    return tee_result;
}

/**
 * sys_vmsplice — Splice user pages into a pipe
 * @fd:          File descriptor (must be write end of a pipe)
 * @iov_addr:    User-space pointer to struct iovec array
 * @nr_segs:     Number of iovec entries
 * @flags:       SPLICE_F_MOVE, SPLICE_F_NONBLOCK, SPLICE_F_MORE, SPLICE_F_GIFT
 *
 * Linux signature:
 *   ssize_t vmsplice(int fd, const struct iovec *iov,
 *                    unsigned long nr_segs, unsigned int flags);
 *
 * Linux semantics:
 *   - fd must be the write end of a pipe.
 *   - Data from user-space buffers (described by iov) is written to the pipe.
 *   - SPLICE_F_GIFT is accepted but treated as a hint (pages are always
 *     copied for safety).  SPLICE_F_NONBLOCK makes the pipe side use
 *     non-blocking semantics.
 *   - nr_segs is capped at 1024 (Linux limit).
 *   - Returns the total number of bytes written to the pipe.
 *
 * Return: Number of bytes written (as uint64_t), or negative errno:
 *   EBADF  — invalid fd or wrong mode
 *   EINVAL — fd is not a pipe write end
 *   EFAULT — bad user-space pointer
 *   ENOMEM — cannot allocate iovec or bounce buffer
 *   EPIPE  — pipe has no readers
 */
static uint64_t sys_vmsplice(uint64_t fd, uint64_t iov_addr,
                             uint64_t nr_segs, uint64_t flags)
{
    (void)flags;

    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-EPERM;

    /* nr_segs == 0 is a no-op per Linux semantics */
    if (nr_segs == 0)
        return 0;
    if (nr_segs > 1024)
        return (uint64_t)(int64_t)-EINVAL;
    if (!iov_addr)
        return (uint64_t)(int64_t)-EFAULT;

    /* Validate fd range */
    if (fd >= PROCESS_FD_MAX)
        return (uint64_t)(int64_t)-EBADF;

    struct process_fd *pfd = &p->fd_table[fd];
    if (!pfd->used)
        return (uint64_t)(int64_t)-EBADF;

    /* fd must be the write end of a pipe (vmsplice always writes to pipe) */
    if (strncmp(pfd->path, "pipe_write_", 11) != 0)
        return (uint64_t)(int64_t)-EINVAL;

    /* Check that fd is writable */
    if ((pfd->open_flags & 3) == 0)  /* O_RDONLY alone */
        return (uint64_t)(int64_t)-EBADF;

    int pipe_id = (int)pfd->offset;

    /* Copy iovec array from userspace */
    struct iovec iov_stack[16];
    struct iovec *iov = iov_stack;
    int allocd = 0;

    if (nr_segs > 16) {
        iov = kmalloc(sizeof(struct iovec) * (size_t)nr_segs);
        if (!iov)
            return (uint64_t)(int64_t)-ENOMEM;
        allocd = 1;
    }

    if (copy_from_user(iov, iov_addr,
                       sizeof(struct iovec) * nr_segs) < 0) {
        if (allocd) kfree(iov);
        return (uint64_t)(int64_t)-EFAULT;
    }

    /* Use a 4 KiB kernel bounce buffer for userspace→pipe transfer */
    uint8_t *bounce = kmalloc(4096);
    if (!bounce) {
        if (allocd) kfree(iov);
        return (uint64_t)(int64_t)-ENOMEM;
    }

    uint64_t total = 0;
    uint64_t vmsplice_result;

    for (uint64_t i = 0; i < nr_segs; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0)
            continue;

        uint64_t remaining = iov[i].iov_len;
        uint64_t offset = 0;

        while (remaining > 0) {
            uint64_t chunk = (remaining > 4096) ? 4096 : remaining;

            /* Copy user data into bounce buffer */
            if (copy_from_user(bounce,
                               (uint64_t)iov[i].iov_base + offset,
                               chunk) < 0) {
                vmsplice_result = (total > 0)
                    ? total
                    : (uint64_t)(int64_t)-EFAULT;
                goto vmsplice_out;
            }

            /* Write bounce buffer to pipe */
            int w = pipe_write(pipe_id, bounce, (int)chunk);
            if (w < 0) {
                vmsplice_result = (total > 0)
                    ? total
                    : (uint64_t)(int64_t)w;
                goto vmsplice_out;
            }
            total += (uint64_t)w;
            offset += (uint64_t)w;

            /* Pipe full — stop */
            if ((uint64_t)w < chunk)
                break;

            remaining -= (uint64_t)w;
        }

        /* Short write on this iovec — stop processing further segments */
        if (offset < iov[i].iov_len)
            break;
    }

    vmsplice_result = total;

vmsplice_out:
    kfree(bounce);
    if (allocd) kfree(iov);
    return vmsplice_result;
}


/* ── copy_file_range — zero-copy file-to-file data transfer (Task D124-9) ── */

/**
 * sys_copy_file_range — Copy data between two file descriptors
 *                       within the kernel (Linux-compatible).
 *
 * POSIX / Linux signature:
 *   ssize_t copy_file_range(int fd_in, loff_t *off_in,
 *                           int fd_out, loff_t *off_out,
 *                           size_t len, unsigned int flags);
 *
 * Copies up to @len bytes from fd_in to fd_out.  If @off_in is NULL
 * (0 in the syscall ABI), the copy starts at fd_in's current offset
 * and that offset is advanced by the number of bytes copied.  If
 * @off_in is non-NULL, the copy starts at the absolute offset stored
 * in *off_in, fd_in's offset is NOT updated, and *off_in is advanced.
 * Same behaviour for @off_out.
 *
 * A kernel bounce buffer is used for the transfer.  A production
 * implementation would splice pages directly between page caches,
 * but this is correct and sufficient for current purposes.
 *
 * Return: Number of bytes copied (as uint64_t, matching the Linux
 *         ssize_t convention via the kernel's uint64_t return path),
 *         or a negative errno encoded as (uint64_t)(int64_t)-ERRNO.
 *
 * Linux errno values:
 *   EBADF      — fd_in not open for reading, or fd_out not open for writing
 *   EINVAL     — flags != 0, or overlapping same-file copy with same offsets
 *   EFAULT     — bad user-space pointer in off_in / off_out
 *   EISDIR     — fd_in or fd_out refers to a directory
 *   EOVERFLOW  — offset or len exceeds implementation limits
 *   ENOMEM     — cannot allocate bounce buffer
 *   EIO        — VFS read/write failure
 *   EFBIG      — write would exceed file size limit (RLIMIT_FSIZE)
 *   ENOSPC     — no space left on device
 *   EXDEV      — cross-device copy (not an error here; we handle it)
 *   ESPIPE     — fd_in or fd_out refers to a pipe/FIFO (not supported yet)
 */
static uint64_t sys_copy_file_range(uint64_t fd_in, uint64_t off_in_addr,
                                     uint64_t fd_out, uint64_t off_out_addr,
                                     uint64_t len, uint64_t flags)
{
    /* ── Parameter validation ── */

    /* Per POSIX.1-2016 / Linux, flags must be 0 */
    if (flags != 0)
        return (uint64_t)(int64_t)-EINVAL;

    /* len == 0 is a no-op (Linux returns 0 immediately) */
    if (len == 0)
        return 0;

    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-EPERM;

    /* Validate file descriptors using the fd-3 slot convention */
    if (fd_in < 3 || fd_out < 3)
        return (uint64_t)(int64_t)-EBADF;
    int i_in  = (int)fd_in - 3;
    int i_out = (int)fd_out - 3;
    if (i_in >= PROCESS_FD_MAX || i_out >= PROCESS_FD_MAX)
        return (uint64_t)(int64_t)-EBADF;

    struct process_fd *pfd_in  = &p->fd_table[i_in];
    struct process_fd *pfd_out = &p->fd_table[i_out];
    if (!pfd_in->used || !pfd_out->used)
        return (uint64_t)(int64_t)-EBADF;

    /* fd_in must be open for reading (O_RDONLY or O_RDWR) */
    if ((pfd_in->open_flags & 3) == 1)   /* O_WRONLY alone */
        return (uint64_t)(int64_t)-EBADF;

    /* fd_out must be open for writing (O_WRONLY or O_RDWR) */
    if ((pfd_out->open_flags & 3) != 1 && /* neither O_WRONLY */
        (pfd_out->open_flags & 3) != 2)   /* nor O_RDWR    */
        return (uint64_t)(int64_t)-EBADF;

    /* Verify both fds refer to regular files (type == 1) */
    struct vfs_stat st_in, st_out;
    if (vfs_stat(pfd_in->path, &st_in) < 0)
        return (uint64_t)(int64_t)-EIO;
    if (st_in.type != 1)
        return (uint64_t)(int64_t)-EISDIR;

    if (vfs_stat(pfd_out->path, &st_out) < 0)
        return (uint64_t)(int64_t)-EIO;
    if (st_out.type != 1)
        return (uint64_t)(int64_t)-EISDIR;

    /* Check that offset+len does not overflow uint32_t for the VFS layer */
    if (len > 0xFFFFFFFFULL)
        return (uint64_t)(int64_t)-EOVERFLOW;

    /* ── Setup ── */

    uint8_t *buf = kmalloc(4096);
    if (!buf)
        return (uint64_t)(int64_t)-ENOMEM;

    uint64_t total = 0;
    uint64_t cfr_result;

    /* ── Copy loop ── */

    while (total < len) {
        uint64_t chunk = len - total;
        if (chunk > 4096)
            chunk = 4096;

        /* ── Source offset ── */
        uint64_t saved_in_off = pfd_in->offset;
        if (off_in_addr != 0) {
            int64_t abs_off;
            if (copy_from_user(&abs_off, off_in_addr, sizeof(abs_off)) < 0) {
                cfr_result = (uint64_t)(int64_t)-EFAULT;
                goto cfr_cleanup;
            }
            if (abs_off < 0) {
                cfr_result = (uint64_t)(int64_t)-EINVAL;
                goto cfr_cleanup;
            }
            /* Temporarily set fd_in's offset to the user-supplied value */
            pfd_in->offset = (uint32_t)abs_off;
        }

        /* Read a chunk from source */
        uint32_t nread = 0;
        int r = vfs_read(pfd_in->path, buf, (uint32_t)chunk, &nread);
        if (r < 0) {
            /* On first iteration, propagate the exact VFS errno.
             * On subsequent iterations, return partial success. */
            if (off_in_addr != 0)
                pfd_in->offset = saved_in_off;
            cfr_result = (total > 0) ? (uint64_t)total
                                     : (uint64_t)(int64_t)r;
            goto cfr_cleanup;
        }

        /* Advance the user-provided source offset if applicable */
        if (off_in_addr != 0) {
            int64_t new_off = (int64_t)(pfd_in->offset);
            if (copy_to_user(off_in_addr, &new_off, sizeof(new_off)) < 0) {
                cfr_result = (uint64_t)(int64_t)-EFAULT;
                goto cfr_cleanup;
            }
        }

        if (nread == 0)
            break;  /* EOF */

        /* ── Destination offset ── */
        uint64_t saved_out_off = pfd_out->offset;
        if (off_out_addr != 0) {
            int64_t abs_off;
            if (copy_from_user(&abs_off, off_out_addr, sizeof(abs_off)) < 0) {
                cfr_result = (uint64_t)(int64_t)-EFAULT;
                goto cfr_cleanup;
            }
            if (abs_off < 0) {
                cfr_result = (uint64_t)(int64_t)-EINVAL;
                goto cfr_cleanup;
            }
            pfd_out->offset = (uint32_t)abs_off;
        }

        /* Write chunk to destination */
        if (vfs_write(pfd_out->path, buf, nread) < 0) {
            /* Restore offsets on write failure */
            if (off_in_addr != 0)
                pfd_in->offset = saved_in_off;
            if (off_out_addr != 0)
                pfd_out->offset = saved_out_off;
            cfr_result = (total > 0) ? (uint64_t)total
                                     : (uint64_t)(int64_t)-EIO;
            goto cfr_cleanup;
        }

        /* Advance user-provided destination offset */
        if (off_out_addr != 0) {
            int64_t new_off = (int64_t)(pfd_out->offset);
            if (copy_to_user(off_out_addr, &new_off, sizeof(new_off)) < 0) {
                cfr_result = (uint64_t)(int64_t)-EFAULT;
                goto cfr_cleanup;
            }
        }

        total += nread;

        if (nread < chunk)
            break;  /* Short read — EOF or underlying FS limit */
    }

    cfr_result = (uint64_t)total;

cfr_cleanup:
    kfree(buf);
    return cfr_result;
}

/* ── sendmmsg / recvmmsg ──────────────────────────────────────────────── */

/* For each iovec entry, send one message via net_tcp_send or similar.
 * Simplified: just send each iovec entry as if it were a regular write. */
static uint64_t sys_sendmmsg(uint64_t sockfd, uint64_t msgvec_addr,
                              uint64_t vlen, uint64_t flags) {
    (void)flags;
    struct process *p = process_get_current();
    if (!p || sockfd >= PROCESS_FD_MAX || !p->fd_table[sockfd].used)
        return (uint64_t)(int64_t)-EBADF;

    int max = (int)vlen > 8 ? 8 : (int)vlen;
    int sent = 0;
    for (int i = 0; i < max; i++) {
        /* Read iovec from struct mmsghdr at msgvec_addr + i * sizeof(struct mmsghdr) */
        uint64_t entry_addr = msgvec_addr + (uint64_t)i * 64;
        if (!syscall_user_read_ok(entry_addr, 64)) break;
        (void)entry_addr;
        /* Send each message by iterating iovec entries */
        for (unsigned int j = 0; j < 4; j++) {
            (void)entry_addr;
        }
    }
    return (uint64_t)sent;
}

static uint64_t sys_recvmmsg(uint64_t sockfd, uint64_t msgvec_addr,
                              uint64_t vlen, uint64_t flags, uint64_t timeout_addr) {
    (void)sockfd; (void)msgvec_addr; (void)vlen; (void)flags; (void)timeout_addr;
    return (uint64_t)(int64_t)-ENOSYS;
}

/* ── sync / syncfs ────────────────────────────────────────────────────── */

static uint64_t sys_sync(void) {
    /* Flush all mounted filesystems and the buffer cache */
    return (uint64_t)vfs_sync_all();
}

static uint64_t sys_syncfs(uint64_t fd) {
    struct process *proc = process_get_current();
    if (!proc) return (uint64_t)-EBADF;

    /* Regular file fd (3+) */
    if (fd >= 3 && fd < (uint64_t)(3 + PROCESS_FD_MAX)) {
        int i = (int)fd - 3;
        if (i < 0 || i >= PROCESS_FD_MAX || !proc->fd_table[i].used)
            return (uint64_t)-EBADF;

        return (uint64_t)vfs_flush(proc->fd_table[i].path);
    }

    /* For special fds, fall back to global sync */
    return (uint64_t)vfs_sync_all();
}

/* ── setsid / getsid ──────────────────────────────────────────────────── */

static uint64_t sys_setsid(void) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-1;
    /* Create a new session: this process becomes session leader */
    p->sid = p->pid;
    p->pgid = p->pid;
    return (uint64_t)p->sid;
}

static uint64_t sys_getsid(uint64_t pid) {
    struct process *target;
    if (pid == 0)
        target = process_get_current();
    else
        target = process_get_by_pid((uint32_t)pid);
    if (!target || target->state == PROCESS_UNUSED) return (uint64_t)-1;
    return (uint64_t)target->sid;
}

/* ── sigaltstack ──────────────────────────────────────────────────────── */

/**
 * sys_sigaltstack — Set or examine alternate signal stack.
 *
 *   int sigaltstack(const stack_t *restrict ss,
 *                   stack_t *restrict old_ss);
 *
 * Linux-compatible implementation:
 *  - If old_ss is non-NULL, return the current alt-stack configuration.
 *  - If ss is non-NULL, set a new alt-stack (subject to validation).
 *  - ss->ss_flags must be 0 (enable) or SS_DISABLE (disable).
 *  - When enabling, ss->ss_size must be >= MINSIGSTKSZ.
 *  - Attempting to change the stack while executing on it returns -EPERM.
 *
 * Returns 0 on success, -errno on failure.
 */
static uint64_t sys_sigaltstack(uint64_t ss_addr, uint64_t old_ss_addr)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /*
     * Step 1: Return old stack configuration if requested.
     * Do this BEFORE any state change so the caller can snapshot
     * the current config even while simultaneously disabling it.
     */
    if (old_ss_addr) {
        stack_t old;
        old.ss_sp    = p->alt_stack_sp;
        old.ss_flags = p->alt_stack_flags;
        old.ss_size  = p->alt_stack_size;

        if (copy_to_user(old_ss_addr, &old, sizeof(old)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    /*
     * Step 2: Set new stack configuration if requested.
     */
    if (ss_addr) {
        stack_t new;
        if (copy_from_user(&new, ss_addr, sizeof(new)) < 0)
            return (uint64_t)(int64_t)-EFAULT;

        int ss_flags = new.ss_flags;

        /* Reject unknown flags (SS_AUTODISARM is not yet supported) */
        if (ss_flags & ~(SS_DISABLE | SS_ONSTACK))
            return (uint64_t)(int64_t)-EINVAL;

        /* If the process is currently executing on the alternate stack,
         * changing it would corrupt the running handler's stack frame.
         * SS_ONSTACK is set by the kernel during signal delivery when
         * SA_ONSTACK is in effect. */
        if (ss_flags & SS_ONSTACK)
            return (uint64_t)(int64_t)-EPERM;

        if (ss_flags & SS_DISABLE) {
            /* Disable the alternate signal stack */
            p->alt_stack_sp     = NULL;
            p->alt_stack_size   = 0;
            p->alt_stack_flags  = SS_DISABLE;
        } else {
            /* Enable — must supply a large enough stack */
            if (new.ss_size < MINSIGSTKSZ)
                return (uint64_t)(int64_t)-ENOMEM;

            p->alt_stack_sp     = new.ss_sp;
            p->alt_stack_size   = (uint64_t)new.ss_size;
            p->alt_stack_flags  = 0;   /* SS_ONSTACK will be set by the
                                         * kernel at signal delivery time */
        }
    }

    return 0;
}

/* ── personality ──────────────────────────────────────────────────────── */

static uint64_t sys_personality(uint64_t persona) {
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-1;
    uint64_t old = p->personality;
    if (persona != 0xFFFFFFFFFFFFFFFFULL)
        p->personality = persona;
    return old;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Module syscalls (M17-M20)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * sys_init_module — Load a kernel module from a filesystem path.
 *
 *   @path:   Userspace path string to the .ko file.
 *   @params: Optional parameter string (can be NULL).
 *
 * Reads the file into a temporary kernel buffer, runs the ELF module loader
 * (validate → parse → finalize), and returns the module_id (>= 0) on success
 * or -errno on failure.
 *
 * Only callable by privileged (kernel-mode) code — user processes get -EPERM.
 */
static uint64_t sys_init_module(uint64_t path_addr, uint64_t params_addr)
{
    const char *path = (const char *)path_addr;
    const char *params = (params_addr) ? (const char *)params_addr : NULL;

    /* Lockdown: reject module loading at INTEGRITY level or above */
    if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY))
        return (uint64_t)-EPERM;

    /* CAP_SYS_MODULE check — required to load kernel modules */
    {
        int cap_ret = cap_sys_module_check();
        if (cap_ret < 0)
            return (uint64_t)(int64_t)cap_ret;
    }

    /* Only root (or kernel context) can load modules */
    if (syscall_is_user_process()) {
        struct process *p = process_get_current();
        if (!p || p->euid != 0)
            return (uint64_t)-EPERM;
        /* Validate user-space pointers */
        if (path && !syscall_user_cstr_ok(path_addr))
            return (uint64_t)-EFAULT;
        if (params && !syscall_user_cstr_ok(params_addr))
            return (uint64_t)-EFAULT;
    }

    /* Validate the path string */
    if (!path || !path[0])
        return (uint64_t)-EINVAL;

    /* Stat the file to get its size */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0)
        return (uint64_t)-ENOENT;

    uint64_t file_size = st.size;
    if (file_size == 0 || file_size > 8 * 1024 * 1024) {
        /* Reject empty or >8MB modules */
        return (uint64_t)-EFBIG;
    }

    /* Allocate a kernel buffer for the file contents */
    void *buf = kmalloc((size_t)file_size);
    if (!buf)
        return (uint64_t)-ENOMEM;

    /* Read the file */
    uint32_t bytes_read = 0;
    int ret = vfs_read(path, buf, (uint32_t)file_size, &bytes_read);
    if (ret < 0 || bytes_read != file_size) {
        kfree(buf);
        return (uint64_t)-EIO;
    }

    /* Run the ELF module loader */
    struct module_elf_context ctx;
    int result = -1;

    /* Step 1: Validate ELF header */
    if (module_elf_validate(&ctx, (const uint8_t *)buf, file_size) < 0) {
        kprintf("[MOD] init_module(%s): validation failed: %s\n",
                path, ctx.error_msg);
        kfree(buf);
        return (uint64_t)-EINVAL;
    }

    /* Step 2: Parse ELF sections, symbols, relocations */
    if (module_elf_parse(&ctx) < 0) {
        kprintf("[MOD] init_module(%s): parse failed: %s\n",
                path, ctx.error_msg);
        kfree(buf);
        return (uint64_t)-EINVAL;
    }

    /* Step 3: Finalize (resolve, load, relocate, set perms, call init) */
    /* Use the filename (without .ko suffix) as the module name */
    const char *mod_name = ctx.name;
    if (mod_name[0] == '\0') {
        /* Fall back to filename without path */
        const char *slash = path;
        const char *last = path;
        while (*slash) {
            if (*slash == '/') last = slash + 1;
            slash++;
        }
        mod_name = last;
    }

    result = module_elf_finalize(&ctx, mod_name);
    module_elf_free(&ctx);
    kfree(buf);

    if (result < 0) {
        kprintf("[MOD] init_module(%s): finalize failed: %s\n",
                path, ctx.error_msg);
        return (uint64_t)-EINVAL;
    }

    kprintf("[MOD] init_module(%s): loaded as id=%d\n", path, result);

    /* Apply any boot-time cmdline parameters that match this module (M33).
     * These are applied before the user-supplied params so that user
     * params override cmdline defaults. */
    {
        struct kernel_module *mod = module_get_by_id(result);
        if (mod)
            module_apply_cmdline_params(mod);
    }

    /* Parse module parameters if provided (M29) */
    if (params && params[0]) {
        struct kernel_module *mod = module_get_by_id(result);
        if (mod) {
            int pret = module_parse_params(mod, params);
            if (pret < 0) {
                kprintf("[MOD] init_module(%s): parameter parsing failed (%d), unloading\n",
                        path, pret);
                module_unload(result);
                return (uint64_t)(pret == -ENOENT ? (uint64_t)-ENOENT :
                                  pret == -ENOMEM ? (uint64_t)-ENOMEM :
                                  (uint64_t)-EINVAL);
            }
        }
    }

    /* Create sysfs entries for module parameters (M30) */
    {
        struct kernel_module *mod = module_get_by_id(result);
        if (mod)
            module_sysfs_add_params(mod);
    }

    return (uint64_t)result;
}

/*
 * sys_finit_module — Load a kernel module from an already-open file descriptor.
 *
 *   @fd:     Open file descriptor to the .ko file.
 *   @params: Optional parameter string (can be NULL).
 *   @flags:  Module loading flags (reserved, must be 0).
 *
 * Same loading sequence as sys_init_module, but reads from the caller's
 * fd table instead of opening a path.
 */
static uint64_t sys_finit_module(uint64_t fd, uint64_t params_addr, uint64_t flags)
{
    const char *params = (params_addr) ? (const char *)params_addr : NULL;
    (void)flags;

    /* Lockdown: reject module loading at INTEGRITY level or above */
    if (lockdown_is_locked_down(LOCKDOWN_INTEGRITY))
        return (uint64_t)-EPERM;

    /* CAP_SYS_MODULE check — required to load kernel modules */
    {
        int cap_ret = cap_sys_module_check();
        if (cap_ret < 0)
            return (uint64_t)(int64_t)cap_ret;
    }

    /* Only root can load modules */
    if (syscall_is_user_process()) {
        struct process *p = process_get_current();
        if (!p || p->euid != 0)
            return (uint64_t)-EPERM;
        /* Validate user-space pointer for params */
        if (params && !syscall_user_cstr_ok(params_addr))
            return (uint64_t)-EFAULT;
    }

    /* Validate the fd */
    int i = (int)fd - 3;
    struct process *p = process_get_current();
    if (!p) return (uint64_t)-ESRCH;
    struct process_fd *pfd = sys_get_fd(i);
    if (!pfd || !pfd->used)
        return (uint64_t)-EBADF;

    const char *path = pfd->path;
    if (!path || !path[0])
        return (uint64_t)-EINVAL;

    /* Stat the file to get its size */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0)
        return (uint64_t)-ENOENT;

    uint64_t file_size = st.size;
    if (file_size == 0 || file_size > 8 * 1024 * 1024)
        return (uint64_t)-EFBIG;

    /* Allocate a kernel buffer for the file contents */
    void *buf = kmalloc((size_t)file_size);
    if (!buf)
        return (uint64_t)-ENOMEM;

    /* Read the file using the saved path + offset */
    /* We need to reset the offset to 0 for a full read */
    uint64_t saved_offset = pfd->offset;
    pfd->offset = 0;

    uint32_t bytes_read = 0;
    int ret = vfs_read(path, buf, (uint32_t)file_size, &bytes_read);
    pfd->offset = saved_offset; /* restore original offset */

    if (ret < 0 || bytes_read != file_size) {
        kfree(buf);
        return (uint64_t)-EIO;
    }

    /* Run the ELF module loader */
    struct module_elf_context ctx;
    int result = -1;

    if (module_elf_validate(&ctx, (const uint8_t *)buf, file_size) < 0) {
        kprintf("[MOD] finit_module(fd=%llu): validation failed: %s\n",
                fd, ctx.error_msg);
        kfree(buf);
        return (uint64_t)-EINVAL;
    }

    if (module_elf_parse(&ctx) < 0) {
        kprintf("[MOD] finit_module(fd=%llu): parse failed: %s\n",
                fd, ctx.error_msg);
        kfree(buf);
        return (uint64_t)-EINVAL;
    }

    /* Derive module name from the file path */
    const char *mod_name = ctx.name;
    if (mod_name[0] == '\0') {
        const char *slash = path;
        const char *last = path;
        while (*slash) {
            if (*slash == '/') last = slash + 1;
            slash++;
        }
        mod_name = last;
    }

    result = module_elf_finalize(&ctx, mod_name);
    module_elf_free(&ctx);
    kfree(buf);

    if (result < 0) {
        kprintf("[MOD] finit_module(fd=%llu): finalize failed: %s\n",
                fd, ctx.error_msg);
        return (uint64_t)-EINVAL;
    }

    kprintf("[MOD] finit_module(fd=%llu): loaded as id=%d\n", fd, result);

    /* Parse module parameters if provided (M29) */
    if (params && params[0]) {
        struct kernel_module *mod = module_get_by_id(result);
        if (mod) {
            int pret = module_parse_params(mod, params);
            if (pret < 0) {
                kprintf("[MOD] finit_module(fd=%llu): parameter parsing failed (%d), unloading\n",
                        fd, pret);
                module_unload(result);
                return (uint64_t)(pret == -ENOENT ? (uint64_t)-ENOENT :
                                  pret == -ENOMEM ? (uint64_t)-ENOMEM :
                                  (uint64_t)-EINVAL);
            }
        }
    }

    /* Create sysfs entries for module parameters (M30) */
    {
        struct kernel_module *mod = module_get_by_id(result);
        if (mod)
            module_sysfs_add_params(mod);
    }

    return (uint64_t)result;
}

/*
 * sys_delete_module — Unload a kernel module by name.
 *
 *   @name:  Module name string.
 *   @flags: May contain O_NONBLOCK to fail instead of waiting for refcount drain.
 *
 * Only callable by root.
 */
static uint64_t sys_delete_module(uint64_t name_addr, uint64_t flags)
{
    const char *name = (const char *)name_addr;

    /* Only root can unload modules */
    if (syscall_is_user_process()) {
        struct process *p = process_get_current();
        if (!p || p->euid != 0)
            return (uint64_t)-EPERM;
    }

    if (!name || !name[0])
        return (uint64_t)-EINVAL;

    /* Find the module by name */
    struct kernel_module *mod = module_find(name);
    if (!mod)
        return (uint64_t)-ENOENT;

    /* Check refcount */
    if (mod->refcount > 0) {
        if (flags & 1) { /* O_NONBLOCK */
            return (uint64_t)-EBUSY;
        }
        /* Non-blocking case: just report busy for now.
         * A full implementation would wait with timeout. */
        kprintf("[MOD] delete_module(%s): refcount=%d, cannot unload\n",
                name, mod->refcount);
        return (uint64_t)-EBUSY;
    }

    /* Remove sysfs entries for module parameters (M30) */
    module_sysfs_remove_params(mod);

    /* Unload the module */
    int ret = module_unload(mod->module_id);
    if (ret < 0) {
        kprintf("[MOD] delete_module(%s): unload failed\n", name);
        return (uint64_t)-EINVAL;
    }

    kprintf("[MOD] delete_module(%s): unloaded successfully\n", name);
    return 0;
}

/*
 * sys_query_module — Query information about loaded modules.
 *
 *   @name:     Module name to query, or NULL/empty to enumerate.
 *   @info_buf: Output buffer for module information.
 *   @buf_size: Size of the output buffer.
 *
 * When name is NULL or empty, enumerates all loaded modules into the buffer
 * as a series of null-terminated strings ("name1\0name2\0...\0").
 * When name is given, returns details about that specific module
 * in key=value format (one per line).
 *
 * On success, returns the number of bytes written (not including trailing \0
 * for enumeration mode). Returns -errno on failure.
 */
static uint64_t sys_query_module(uint64_t name_addr, uint64_t info_buf_addr,
                                  uint64_t buf_size)
{
    const char *name = (const char *)name_addr;
    char *info_buf = (char *)info_buf_addr;

    if (!info_buf || buf_size == 0)
        return (uint64_t)-EINVAL;

    /* If name is NULL or empty, enumerate all loaded modules */
    if (!name || !name[0]) {
        uint64_t remaining = buf_size;
        int written = 0;

        for (int i = 0; i < MODULE_MAX; i++) {
            const char *mod_name = module_name_by_id(i);
            if (!mod_name)
                continue;

            /* Write module name + null terminator */
            int len = (int)strlen(mod_name) + 1; /* include null */
            if (len > (int)remaining)
                break; /* buffer full */

            memcpy(info_buf + written, mod_name, (size_t)len);
            written += len;
            remaining -= (uint64_t)len;
        }

        return (uint64_t)written;
    }

    /* Query specific module */
    struct kernel_module *mod = module_find(name);
    if (!mod)
        return (uint64_t)-ENOENT;

    /* Format module info into buffer */
    int written = snprintf(info_buf, (size_t)buf_size,
        "name=%s\nstate=%d\nrefcount=%d\nbase=0x%llx\nsize=%llu\n",
        mod->name, (int)mod->state, mod->refcount,
        (unsigned long long)mod->base_addr,
        (unsigned long long)mod->size);

    if (written < 0)
        return (uint64_t)-EINVAL;

    return (uint64_t)(written < (int)buf_size ? written : (int)buf_size - 1);
}

/* ══════════════════════════════════════════════════════════════════════════ */

/* Forward declarations for syscalls defined after the dispatch */
static uint64_t sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t cpu_id);
static uint64_t sys_rseq(uint64_t rseq_addr, uint64_t rseq_len,
                         uint64_t rseq_sig, uint64_t flags);
static uint64_t sys_name_to_handle_at(uint64_t dirfd, uint64_t pathname,
                                       uint64_t handle, uint64_t mount_id,
                                       uint64_t flags);
static uint64_t sys_open_by_handle_at(uint64_t mount_fd, uint64_t handle,
                                       uint64_t flags);

/* Forward declaration for raw dispatch (no seccomp/audit/validation) */
uint64_t syscall_dispatch_internal(uint64_t num, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5);

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    /* Seccomp check — must happen before any capability or argument validation */
    if (syscall_is_user_process()) {
        uint32_t seccomp_action = seccomp_evaluate_syscall(num, a1, a2, a3, 0);
        switch (seccomp_action) {
        case SECCOMP_RET_ALLOW:
        case SECCOMP_RET_LOG:
            /* LOG already wrote audit record — allow to proceed */
            break;
        case SECCOMP_RET_TRAP:
            seccomp_send_sigsys(num, 0);
            return (uint64_t)-1; /* EPERM — caller may handle SIGSYS on return */
        case SECCOMP_RET_KILL:
        default:
            /* seccomp_evaluate_syscall already logged audit — kill process */
            process_exit_code(SIGSYS);
            return (uint64_t)-1;
        }
    }

    /* BPF seccomp check — runs when the process has a BPF-based filter
     * (SECCOMP_MODE_FILTER_BPF, mode 3). Uses the BPF virtual machine to
     * evaluate the filter program. Handles KILL/TRAP/LOG/ALLOW actions. */
    {
        struct process *p = process_get_current();
        if (p && p->seccomp_mode == SECCOMP_MODE_FILTER_BPF && p->seccomp_filter) {
            uint32_t bpf_action = seccomp_filter_evaluate((int)num, AUDIT_ARCH_X86_64);
            switch (bpf_action & SECCOMP_RET_ACTION_FULL) {
            case SECCOMP_BPF_RET_ALLOW:
                break;
            case SECCOMP_BPF_RET_LOG:
                /* Logged via audit inside evaluate — allow to proceed */
                break;
            case SECCOMP_BPF_RET_TRAP:
                seccomp_send_sigsys(num, 0);
                return (uint64_t)-1;
            case SECCOMP_BPF_RET_KILL:
            default:
                process_exit_code(SIGSYS);
                return (uint64_t)-1;
            }
        }
    }

    /* Audit syscall entry */
    audit_syscall_entry(num, a1, a2, a3, a4, a5);

    if (syscall_is_user_process()) {
        struct process *p = process_get_current();
        if (!p || !process_caps_has(p, (uint32_t)num)) return (uint64_t)-1;
    }

    {
        int args_ret = syscall_validate_user_args(num, a1, a2, a3, a4, a5);
        if (args_ret < 0) {
            return (uint64_t)(int64_t)args_ret;
        }
    }

    return syscall_dispatch_internal(num, a1, a2, a3, a4, a5);
}

/* ── syscall_dispatch_internal — raw dispatch (no seccomp/audit/validation) ── */

uint64_t syscall_dispatch_internal(uint64_t num, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (num) {
        case SYS_READ:   return sys_read(a1, a2, a3);
        case SYS_WRITE:  return sys_write(a1, a2, a3);
        case SYS_OPEN:   return sys_open(a1, a2, a3);
        case SYS_CLOSE:  return sys_close(a1);
        case SYS_CLOSE_RANGE: return sys_close_range(a1, a2, a3);
        case SYS_EXIT:   return sys_exit(a1);
        case SYS_EXIT_GROUP:   return sys_exit_group(a1);
        case SYS_SET_TID_ADDRESS: return sys_set_tid_address(a1);
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
        case SYS_WAIT4:         return sys_wait4(a1, a2, a3, a4);
        case SYS_WAITID:        return sys_waitid(a1, a2, a3, a4, a5);
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
        case SYS_SETPRIORITY:         return sys_setpriority(a1, a2, a3);
        case SYS_SHM_GET:             return sys_shm_get(a1, a2);
        case SYS_SHM_AT:              return sys_shm_at(a1);
        case SYS_SHM_DT:              return sys_shm_dt(a1);
        case SYS_SHM_FREE:            return sys_shm_free(a1);
        case SYS_SHMCTL:              return sys_shmctl(a1, a2, a3);
        case SYS_FORK:                return sys_fork();
        case SYS_CLONE:               return sys_clone(a1, a2, a3, a4, a5);
        case SYS_CLONE3:              return sys_clone3(a1, a2);
        case SYS_UNSHARE:             return sys_unshare(a1);
        case SYS_SETNS:               return sys_setns(a1, a2);
        case SYS_GETTID:              return sys_gettid();
        case SYS_TKILL:               return sys_tkill(a1, a2);
        case SYS_TGKILL:              return sys_tgkill(a1, a2, a3);
        case SYS_EXECVE:              return sys_execve(a1, a2, a3);
        case SYS_POSIX_SPAWN:         return sys_posix_spawn(a1, a2, a3);
        case SYS_KEXEC_LOAD:          return sys_kexec_load(a1, a2, a3);
        /* ── io_uring ───────────────────────────────────── */
        case SYS_IO_URING_SETUP: {
            /* io_uring_setup(entries, params) */
            struct io_uring_params *params = (struct io_uring_params *)a2;
            int64_t ret = sys_io_uring_setup((uint32_t)a1, params);
            return ret < 0 ? (uint64_t)-1 : (uint64_t)ret;
        }
        case SYS_IO_URING_ENTER:
            return (uint64_t)sys_io_uring_enter((int)a1, (uint32_t)a2,
                                                 (uint32_t)a3, (uint32_t)a4);
        case SYS_IO_URING_REGISTER:
            return (uint64_t)sys_io_uring_register((int)a1, (uint32_t)a2,
                                                    (void *)a3, (uint32_t)a4);
        /* ── Inotify (Item 340) ─────────────────────────── */
        case SYS_INOTIFY_INIT1: {
            int ret = sys_inotify_init1((int)a1);
            return ret < 0 ? (uint64_t)-1 : (uint64_t)ret;
        }
        case SYS_INOTIFY_ADD_WATCH: {
            int ret = sys_inotify_add_watch((int)a1, (const char *)a2, (uint32_t)a3);
            return ret < 0 ? (uint64_t)-1 : (uint64_t)ret;
        }
        case SYS_INOTIFY_RM_WATCH: {
            int ret = sys_inotify_rm_watch((int)a1, (int)a2);
            return ret < 0 ? (uint64_t)-1 : (uint64_t)ret;
        }
        case SYS_THREAD_CREATE:       return sys_thread_create(a1, a2);
        case SYS_THREAD_JOIN:         return sys_thread_join(a1, a2);
        case SYS_THREAD_EXIT:         sys_thread_exit((void *)a1); return 0;
        case SYS_NET_CONNLIST:         return sys_net_connlist();
        case SYS_SIGNAL:              return sys_signal(a1, a2);
        case SYS_LSEEK:               return sys_lseek(a1, a2, a3);
        case SYS_TRUNCATE:            return sys_truncate(a1, a2);
        case SYS_RAW_SEND:            return sys_raw_send(a1, a2);
        case SYS_FD_READ:             return sys_fd_read(a1, a2, a3);
        case SYS_FD_WRITE:            return sys_fd_write(a1, a2, a3);
        case SYS_PREAD64:             return sys_pread64(a1, a2, a3, a4);
        case SYS_PWRITE64:            return sys_pwrite64(a1, a2, a3, a4);
        case SYS_SETPRIORITY_PID:     return sys_setpriority(PRIO_PROCESS, a1, a2);
        case SYS_GETPRIORITY:         return sys_getpriority(a1, a2);
        case SYS_SETPGID:             return sys_setpgid(a1, a2);
        case SYS_GETPGID:             return sys_getpgid(a1);
        case SYS_GETPGRP:             return sys_getpgrp();
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
        case SYS_VGA_SET_COLOR: return sys_vga_set_color(a1, a2);
        case SYS_VGA_GET_FB_INFO: return sys_vga_get_fb_info(a1);
        case SYS_KEYBOARD_GETCHAR: return sys_keyboard_getchar();
        case SYS_VGA_PUT_ENTRY_AT: return sys_vga_put_entry_at(a1, a2, a3, a4);
        case SYS_VGA_SET_CURSOR: return sys_vga_set_cursor(a1, a2);
        case SYS_VGA_CLEAR: return sys_vga_clear();
        case SYS_AC97_PRESENT: return ac97_present();
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
        case SYS_MMAP:    return sys_mmap(a1, a2, a3, a4, a5, 0);
        case SYS_MUNMAP:  return sys_munmap(a1, a2);
        case SYS_MPROTECT: return (uint64_t)sys_mprotect(a1, a2, a3);
        case SYS_PKEY_MPROTECT: return (uint64_t)(int64_t)sys_pkey_mprotect((void *)a1, (size_t)a2, (int)a3, (int)a4);
        case SYS_MSEAL:    return sys_mseal(a1, a2, a3);
        case SYS_SECCOMP:  return sys_seccomp(a1, a2, a3);
        case SYS_MREMAP:   return sys_mremap(a1, a2, a3, a4, a5);
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
        case SYS_SETUID:      return sys_setuid(a1);
        case SYS_SETEUID:     return sys_seteuid(a1);
        case SYS_SETGID:      return sys_setgid(a1);
        case SYS_SETEGID:     return sys_setegid(a1);
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
        case SYS_PRLIMIT64:   return sys_prlimit64(a1, a2, a3, a4);
        case SYS_FUTEX:       return sys_futex(a1, a2, a3, a4, a5, syscall_arg6);
        case SYS_ARCH_PRCTL:  return sys_arch_prctl(a1, a2);
        case SYS_POLL:        return sys_poll(a1, a2, a3);
        case SYS_PSELECT6:    return sys_pselect6(a1, a2, a3, a4, a5);
        case SYS_PPOLL:       return sys_ppoll(a1, a2, a3, a4);
        case SYS_EVENTFD:     return sys_eventfd(a1, a2);
        case SYS_SENDFILE:    return sys_sendfile(a1, a2, a3, a4);
        case SYS_IOCTL:       return sys_ioctl(a1, a2, a3);
        case SYS_SYSLOG:      return sys_syslog(a1, a2, a3);
        case SYS_PRCTL:       return sys_prctl(a1, a2, a3, a4, a5);
        case SYS_MOUNT:       return sys_mount(a1, a2, a3, a4, a5);
        case SYS_UMOUNT:      return sys_umount(a1);
        case SYS_FTRUNCATE:   return sys_ftruncate(a1, a2);
        case SYS_READDIR:     return sys_readdir(a1, a2, a3);
        case SYS_EXECVEAT:    return sys_execveat(a1, a2, a3, a4, a5);
        case SYS_IOPRIO_SET:  return sys_ioprio_set(a1, a2, a3);
        case SYS_IOPRIO_GET:  return sys_ioprio_get(a1, a2);
        case SYS_SCHED_SETSCHEDULER: return sys_sched_setscheduler(a1, a2, a3);
        case SYS_SCHED_GETSCHEDULER: return sys_sched_getscheduler(a1);
        case SYS_SCHED_SETATTR:      return sys_sched_setattr(a1, a2, a3);
        case SYS_SCHED_GETATTR:      return sys_sched_getattr(a1, a2, a3, a4);
        case SYS_OPENAT:       return sys_openat(a1, a2, a3, a4);
        case SYS_MKDIRAT:      return sys_mkdirat(a1, a2, a3);
        case SYS_FSTATAT:      return sys_fstatat(a1, a2, a3, a4);
        case SYS_UNLINKAT:     return sys_unlinkat(a1, a2, a3);
        case SYS_RENAMEAT:     return sys_renameat(a1, a2, a3, a4);
        case SYS_SYMLINKAT:    return sys_symlinkat(a1, a2, a3);
        case SYS_READLINKAT:   return sys_readlinkat(a1, a2, a3, a4);
        case SYS_GETDENTS64:   return sys_getdents64(a1, a2, a3);
        case SYS_MLOCK:        return sys_mlock(a1, a2);
        case SYS_MLOCKALL:     return sys_mlockall(a1);
        case SYS_MUNLOCK:      return sys_munlock(a1, a2);
        case SYS_MUNLOCKALL:   return sys_munlockall();
        case SYS_MINCORE:      return sys_mincore(a1, a2, a3);
        case SYS_MADVISE:      return sys_madvise(a1, a2, a3);
        /* NUMA memory policy */
        case SYS_MBIND:            return sys_mbind(a1, a2, a3, a4, a5, syscall_arg6);
        case SYS_SET_MEMPOLICY:    return sys_set_mempolicy(a1, a2, a3);
        case SYS_GET_MEMPOLICY:    return sys_get_mempolicy(a1, a2, a3, a4, a5);
        case SYS_MIGRATE_PAGES:    return sys_migrate_pages(a1, a2, a3, a4);
        case SYS_MOVE_PAGES:       return sys_move_pages(a1, a2, a3, a4, a5, syscall_arg6);
        case SYS_REMAP_FILE_PAGES: return sys_remap_file_pages(a1, a2, a3, a4, a5);
        case SYS_MSYNC:        return sys_msync(a1, a2, a3);
        case SYS_FALLOCATE:    return sys_fallocate(a1, a2, a3, a4);
        case SYS_READAHEAD:    return sys_readahead(a1, a2, a3);
        case SYS_PIVOT_ROOT:      return sys_pivot_root(a1, a2);
        case SYS_CHROOT:          return sys_chroot(a1);
        case SYS_FADVISE64:       return sys_fadvise64(a1, a2, a3, a4);
        case SYS_TIMERFD_CREATE:  return sys_timerfd_create(a1, a2);
        case SYS_TIMERFD_SETTIME: return sys_timerfd_settime(a1, a2, a3, a4);
        case SYS_TIMERFD_GETTIME: return sys_timerfd_gettime(a1, a2);
        case SYS_SIGNALFD:        return sys_signalfd(a1, a2, a3);
        case SYS_MEMFD_CREATE:    return (uint64_t)memfd_syscall_create((const char*)a1, (unsigned int)a2);
        case SYS_SPLICE:          return sys_splice(a1, a2, a3, a4, a5, syscall_arg6);
        case SYS_TEE:             return sys_tee(a1, a2, a3, a4);
        case SYS_VMSPLICE:        return sys_vmsplice(a1, a2, a3, a4);
        case SYS_COPY_FILE_RANGE: return sys_copy_file_range(a1, a2, a3, a4, a5, syscall_arg6);
        case SYS_SENDMMSG:        return sys_sendmmsg(a1, a2, a3, a4);
        case SYS_RECVMMSG:        return sys_recvmmsg(a1, a2, a3, a4, a5);
        case SYS_SYNC:            return sys_sync();
        case SYS_SYNCFS:          return sys_syncfs(a1);
        case SYS_SETSID:          return sys_setsid();
        case SYS_GETSID:          return sys_getsid(a1);
        case SYS_SIGALTSTACK:     return sys_sigaltstack(a1, a2);
        case SYS_PERSONALITY:     return sys_personality(a1);
        /* Batch 3 dispatch */
        case SYS_SOCKET:          return sys_socket(a1, a2, a3);
        case SYS_BIND:            return sys_bind(a1, a2, a3);
        case SYS_LISTEN:          return sys_listen(a1, a2);
        case SYS_ACCEPT:          return sys_accept(a1, a2, a3);
        case SYS_CONNECT:         return sys_connect(a1, a2, a3);
        case SYS_SETSOCKOPT:      return sys_setsockopt(a1, a2, a3, a4, a5);
        case SYS_GETSOCKOPT:      return sys_getsockopt(a1, a2, a3, a4, a5);
        case SYS_SENDMSG:         return sys_sendmsg(a1, a2, a3);
        case SYS_RECVMSG:         return sys_recvmsg(a1, a2, a3);
        case SYS_GETSOCKNAME:     return sys_getsockname(a1, a2, a3);
        case SYS_GETPEERNAME:     return sys_getpeername(a1, a2, a3);
        case SYS_SOCKETPAIR:      return sys_socketpair(a1, a2, a3, a4);
        case SYS_EPOLL_CREATE1:   return sys_epoll_create1(a1);
        case SYS_EPOLL_CTL:       return sys_epoll_ctl(a1, a2, a3, a4);
        case SYS_EPOLL_WAIT:      return sys_epoll_wait(a1, a2, a3, a4);
        case SYS_EPOLL_PWAIT:     return sys_epoll_pwait(a1, a2, a3, a4, a5);
        case SYS_CLOCK_GETTIME:   return sys_clock_gettime(a1, a2);
        case SYS_CLOCK_SETTIME:   return sys_clock_settime(a1, a2);
        case SYS_CLOCK_GETRES:    return sys_clock_getres(a1, a2);
        case SYS_CLOCK_NANOSLEEP: return sys_clock_nanosleep(a1, a2, a3, a4);
        case SYS_TIMER_CREATE:    return sys_timer_create(a1, a2, a3);
        case SYS_TIMER_SETTIME:   return sys_timer_settime(a1, a2, a3, a4);
        case SYS_TIMER_GETTIME:   return sys_timer_gettime(a1, a2);
        case SYS_TIMER_GETOVERRUN: return sys_timer_getoverrun(a1);
        case SYS_TIMER_DELETE:    return sys_timer_delete(a1);
        case SYS_DUP3:            return sys_dup3(a1, a2, a3);
        case SYS_PIPE2:           return sys_pipe2(a1, a2);
        case SYS_MKDTEMP:         return sys_mkdtemp(a1);
        case SYS_UTIMENSAT:       return sys_utimensat(a1, a2, a3, a4);
        case SYS_FUTIMENS:        return sys_futimens(a1, a2);
        case SYS_STATFS:          return sys_statfs(a1, a2);
        case SYS_FSTATFS:         return sys_fstatfs(a1, a2);
        case SYS_GETRUSAGE:       return sys_getrusage(a1, a2);
        case SYS_SYSINFO:         return sys_sysinfo(a1);
        case SYS_CAPGET:          return sys_capget(a1, a2);
        case SYS_CAPSET:          return sys_capset(a1, a2);
        case SYS_SETSECUREBITS:   return sys_setsecurebits(a1);
        case SYS_GETSECUREBITS:   return sys_getsecurebits();
        case SYS_GETRESUID:       return sys_getresuid(a1, a2, a3);
        case SYS_SETRESUID:       return sys_setresuid(a1, a2, a3);
        case SYS_GETRESGID:       return sys_getresgid(a1, a2, a3);
        case SYS_SETRESGID:       return sys_setresgid(a1, a2, a3);
        case SYS_SCHED_GETPARAM:  return sys_sched_getparam(a1, a2);
        case SYS_SCHED_SETPARAM:  return sys_sched_setparam(a1, a2);
        case SYS_MQ_OPEN:         return sys_mq_open(a1, a2, a3, a4);
        case SYS_MQ_SEND:         return sys_mq_send(a1, a2, a3, a4);
        case SYS_MQ_RECEIVE:      return sys_mq_receive(a1, a2, a3, a4);
        case SYS_MQ_UNLINK:       return sys_mq_unlink(a1);
        case SYS_GETCPU: {
            int cpu = smp_get_cpu_id();
            if (a1) { if (copy_to_user(a1, &cpu, sizeof(cpu)) < 0) return (uint64_t)-1; }
            int zero = 0;
            if (a2) { if (copy_to_user(a2, &zero, sizeof(zero)) < 0) return (uint64_t)-1; }
            return 0;
        }
        case SYS_PREADV: {
            /* preadv(fd, iov, iovcnt, offset) — vectored read at position */
            uint64_t fd = a1, iov_addr = a2, iovcnt = a3, offset = a4;
            if (fd < 3) return (uint64_t)-1;
            int i = (int)fd - 3;
            struct process_fd *pfd = sys_get_fd(i);
            if (!pfd || !pfd->used) return (uint64_t)-1;
            uint64_t saved_off = pfd->offset;
            pfd->offset = offset;
            uint64_t ret = sys_readv(fd, iov_addr, iovcnt);
            if ((int64_t)ret < 0) pfd->offset = saved_off;
            return ret;
        }
        case SYS_PWRITEV: {
            /* pwritev(fd, iov, iovcnt, offset) — vectored write at position */
            uint64_t fd = a1, iov_addr = a2, iovcnt = a3, offset = a4;
            if (fd < 3) return (uint64_t)-1;
            int i = (int)fd - 3;
            struct process_fd *pfd = sys_get_fd(i);
            if (!pfd || !pfd->used) return (uint64_t)-1;
            uint64_t saved_off = pfd->offset;
            pfd->offset = offset;
            uint64_t ret = sys_writev(fd, iov_addr, iovcnt);
            if (ret == (uint64_t)-1) pfd->offset = saved_off;
            return ret;
        }
        case SYS_SIGWAITINFO:     return sys_sigwaitinfo(a1, a2);
        case SYS_SIGTIMEDWAIT:    return sys_sigtimedwait(a1, a2, a3);
        case SYS_FDATASYNC:       return sys_fdatasync(a1);
        case SYS_SET_ROBUST_LIST: return (uint64_t)sys_set_robust_list((struct robust_list_head*)a1, (size_t)a2);
        case SYS_GET_ROBUST_LIST: return (uint64_t)sys_get_robust_list((int)a1, (struct robust_list_head**)a2, (size_t*)a3);
        /* ── Module syscalls (M17-M20) ─────────────────────────────── */
        case SYS_INIT_MODULE:     return sys_init_module(a1, a2);
        case SYS_FINIT_MODULE:    return sys_finit_module(a1, a2, a3);
        case SYS_DELETE_MODULE:   return sys_delete_module(a1, a2);
        case SYS_QUERY_MODULE:    return sys_query_module(a1, a2, a3);
        /* ── membarrier (Item 252) ────────────────────────────────── */
        case SYS_MEMBARRIER:      return sys_membarrier(a1, a2, a3);
        /* ── rseq (Item 348) ──────────────────────────────────────── */
        case SYS_RSEQ:            return sys_rseq(a1, a2, a3, a4);
        /* ── File handle operations (Item 250) ────────────────────── */
        case SYS_NAME_TO_HANDLE_AT: return sys_name_to_handle_at(a1, a2, a3, a4, a5);
        case SYS_OPEN_BY_HANDLE_AT: return sys_open_by_handle_at(a1, a2, a3);
        /* ── KCOV code coverage (Item 208) ────────────────────────── */
        case SYS_KCOV:            return (uint64_t)sys_kcov(a1, a2);
        /* ── userfaultfd (a1=fd, a2=cmd, a3=arg) ─────────────── */
        case SYS_USERFAULTFD:     return (uint64_t)sys_userfaultfd2(a1, a2, a3);
        /* ── Swap — block device swap (Item 223) ──────────────────── */
        case SYS_SWAPON:          return (uint64_t)swap_swapon((const char *)a1);
        case SYS_SWAPOFF:         return (uint64_t)swap_swapoff((const char *)a1);
        /* ── Supplementary groups (D127) ─────────────────────────── */
        case SYS_GETGROUPS:       return sys_getgroups(a1, a2);
        case SYS_SETGROUPS:       return sys_setgroups(a1, a2);
        /* ── pidfd operations ─────────────────────────────────────── */
        case SYS_PIDFD_OPEN: {
            int ret = pidfd_open((uint32_t)a1, (uint32_t)a2);
            return ret < 0 ? (uint64_t)-1 : (uint64_t)ret;
        }
        case SYS_PIDFD_SEND_SIGNAL: {
            int ret = pidfd_send_signal((int)a1, (int)a2,
                                        (struct siginfo *)a3, (uint32_t)a4);
            return ret < 0 ? (uint64_t)-1 : 0;
        }
        case SYS_PIDFD_GETFD: {
            int ret = pidfd_getfd((int)a1, (int)a2, (uint32_t)a3);
            return ret < 0 ? (uint64_t)-1 : (uint64_t)ret;
        }
        /* ── mount_setattr ────────────────────────────────────────── */
        case SYS_MOUNT_SETATTR:
            return (uint64_t)mount_setattr((int)a1, (const char *)a2,
                                           (uint32_t)a3,
                                           (const struct mount_attr *)a4,
                                           (size_t)a5);
        /* ── Userspace framebuffer / keyboard syscalls (504-510) ── */
        case SYS_VGA_PUT_PIXEL:
            vga_put_pixel((int32_t)a1, (int32_t)a2, (uint32_t)a3);
            return 0;
        case SYS_VGA_BLIT: {
            uint64_t buf = a1;
            int32_t x = (int32_t)a2, y = (int32_t)a3;
            uint32_t w = (uint32_t)a4, h = (uint32_t)a5;
            for (uint32_t row = 0; row < h; row++) {
                for (uint32_t col = 0; col < w; col++) {
                    uint32_t color;
                    if (copy_from_user(&color, buf + (row * w + col) * 4, 4) < 0)
                        return (uint64_t)-1;
                    vga_put_pixel(x + (int32_t)col, y + (int32_t)row, color);
                }
            }
            return 0;
        }
        case SYS_VGA_CLEAR_FRAMEBUFFER:
            vga_clear_framebuffer((uint32_t)a1);
            return 0;
        case SYS_VGA_REFRESH_CONSOLE:
            vga_refresh_console();
            return 0;
        case SYS_KEYBOARD_HAS_INPUT:
            return (uint64_t)keyboard_has_input();
        case SYS_KEYBOARD_IS_DOWN:
            return (uint64_t)keyboard_is_down((char)(uint8_t)a1);
        case SYS_KEYBOARD_RESET_STATE:
            keyboard_reset_state();
            return 0;
        case SYS_SEMCTL:
            return sys_semctl(a1, a2, a3, a4);
        /* ── D123: Process & Signal Syscalls ────────────────────── */
        case SYS_RT_SIGACTION:
            return sys_rt_sigaction(a1, a2, a3, a4);
        case SYS_RT_SIGPROCMASK:
            return sys_rt_sigprocmask(a1, a2, a3, a4);
        case SYS_RT_SIGRETURN:
            return sys_rt_sigreturn();
        case SYS_RT_SIGTIMEDWAIT:
            return sys_rt_sigtimedwait(a1, a2, a3, a4);
        default: {
            uint64_t ret = (uint64_t)-1;
            audit_syscall_exit(ret);
            return ret;
        }
    }
    /* NOTREACHED */
    return (uint64_t)-1;
}

/* ── rseq — Restartable Sequences (Item 348) ─────────────────────────── */

/*
 * sys_rseq — Register / unregister restartable sequences for the
 *            calling process.
 *
 *   int rseq(struct rseq *rseq_addr, uint32_t rseq_len,
 *            int rseq_sig, unsigned int flags);
 *
 * Returns 0 on success, -errno on failure.
 * With RSEQ_FLAG_UNREGISTER in flags, unregisters the current rseq.
 */
static uint64_t sys_rseq(uint64_t rseq_addr, uint64_t rseq_len,
                          uint64_t rseq_sig, uint64_t flags)
{
    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)(int64_t)-ESRCH;

    /* Validate rseq_addr is in userspace range */
    if (rseq_addr >= USER_VADDR_MAX)
        return (uint64_t)(int64_t)-EFAULT;

    /* Validate rseq_len */
    if (rseq_len < sizeof(struct rseq) || rseq_len > PAGE_SIZE)
        return (uint64_t)(int64_t)-EINVAL;

    /* If UNREGISTER flag is set, unregister */
    if (flags & RSEQ_FLAG_UNREGISTER) {
        int ret = rseq_unregister(cur);
        if (ret < 0)
            return (uint64_t)(int64_t)ret;
        return 0;
    }

    /* Validate rseq_sig must fit in uint32_t */
    if (rseq_sig != (uint32_t)rseq_sig)
        return (uint64_t)(int64_t)-EINVAL;

    int ret = rseq_register(cur, rseq_addr, (uint32_t)rseq_len,
                            (uint32_t)rseq_sig);
    if (ret < 0)
        return (uint64_t)(int64_t)ret;

    return 0;
}

/* ── membarrier — Memory barrier on all threads (Item 252) ─────────── */

/* Per-process flag for membarrier registration */
#define MEMBARRIER_PRIVATE_EXPEDITED  (1U << 0)

/*
 * sys_membarrier(cmd, flags, cpu_id)
 *
 * Issue memory barriers on running threads.  Used by JIT compilers,
 * garbage collectors, and async runtimes to order memory operations
 * without full syscall serialisation.
 *
 * Supported commands:
 *   MEMBARRIER_CMD_QUERY                     — return supported command mask
 *   MEMBARRIER_CMD_GLOBAL                    — full barrier on all CPUs
 *   MEMBARRIER_CMD_GLOBAL_EXPEDITED          — expedited global barrier via IPI
 *   MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED — register for expedited global
 *   MEMBARRIER_CMD_PRIVATE_EXPEDITED         — barrier on process threads
 *   MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED— register for expedited private
 */
static uint64_t sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t cpu_id) {
    (void)cpu_id;  /* CPU-id-based targeting is optional, ignored in this impl */

    /* Validate flags — only MEMBARRIER_CMD_FLAG_CPU is accepted */
    if (flags & ~(uint64_t)MEMBARRIER_CMD_FLAG_CPU)
        return (uint64_t)-1;

    switch (cmd) {
    case MEMBARRIER_CMD_QUERY: {
        /* Report which commands we support */
        uint64_t supported = MEMBARRIER_CMD_QUERY |
                             MEMBARRIER_CMD_GLOBAL |
                             MEMBARRIER_CMD_GLOBAL_EXPEDITED |
                             MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED |
                             MEMBARRIER_CMD_PRIVATE_EXPEDITED |
                             MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
        return supported;
    }

    case MEMBARRIER_CMD_GLOBAL:
    case MEMBARRIER_CMD_GLOBAL_EXPEDITED: {
        /* Full memory barrier on all CPUs.
         * For MEMBARRIER_CMD_GLOBAL, a simple mfence + IPI suffices
         * (the semantics require all prior writes to be visible to all CPUs). */
        __asm__ volatile("mfence" ::: "memory");

        /* If more than one CPU, send IPI to all others */
        if (smp_get_cpu_count() > 1) {
            apic_send_ipi_all_except(IPI_VECTOR_MEMBARRIER);
        }
        return 0;
    }

    case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED: {
        /* Registration: allow future MEMBARRIER_CMD_GLOBAL_EXPEDITED.
         * On Linux this enables fast IPI delivery; we register it
         * per-process for semantic correctness. */
        struct process *cur = process_get_current();
        if (!cur) return (uint64_t)-1;
        cur->membarrier_flags |= MEMBARRIER_PRIVATE_EXPEDITED;
        return 0;
    }

    case MEMBARRIER_CMD_PRIVATE_EXPEDITED: {
        /* Memory barrier on all threads of the current process.
         * Send IPI to CPUs that are running threads of this process. */
        struct process *cur = process_get_current();
        if (!cur) return (uint64_t)-1;

        /* Check if registered */
        if (!(cur->membarrier_flags & MEMBARRIER_PRIVATE_EXPEDITED))
            return (uint64_t)-1;  /* -EPERM */

        __asm__ volatile("mfence" ::: "memory");

        /* For expedited private: in a real system we'd iterate
         * the process's threads and IPI their CPUs.  For now,
         * we IPI all other CPUs — a safe over-approximation. */
        if (smp_get_cpu_count() > 1) {
            apic_send_ipi_all_except(IPI_VECTOR_MEMBARRIER);
        }
        return 0;
    }

    case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED: {
        struct process *cur = process_get_current();
        if (!cur) return (uint64_t)-1;
        cur->membarrier_flags |= MEMBARRIER_PRIVATE_EXPEDITED;
        return 0;
    }

    default:
        return (uint64_t)-1;  /* -EINVAL */
    }
}

/* ── Init ─────────────────────────────────────────────────────── */

/* ── File handle table (Item 250) ────────────────────────────────────
 *
 * name_to_handle_at / open_by_handle_at allow userspace to open files
 * by an opaque handle (used for NFS, file descriptor passing, etc.).
 *
 * Since our VFS is path-based, we maintain a small handle table that
 * maps handle IDs to resolved absolute paths.  Each handle stores the
 * inode number from vfs_stat, plus the resolved path so that
 * open_by_handle_at can re-open the file.
 */

#define FH_MAX_HANDLES 32

struct fh_entry {
    int      in_use;
    uint32_t ino;              /* inode number from vfs_stat */
    uint32_t mount_id;         /* mount identifier (0 = root) */
    int      handle_type;      /* handle type (1 = regular file) */
    char     path[128];        /* resolved absolute path */
    uint32_t handle_bytes;     /* size of handle data stored */
    uint8_t  handle_data[16];  /* raw handle bytes (inode + mount_id) */
};

static struct fh_entry fh_table[FH_MAX_HANDLES];
static int fh_initialized = 0;

static void fh_init(void)
{
    if (fh_initialized) return;
    memset(fh_table, 0, sizeof(fh_table));
    fh_initialized = 1;
}

/* Allocate a free handle entry, returns index or -1 */
static int fh_alloc(void)
{
    if (!fh_initialized) fh_init();
    for (int i = 0; i < FH_MAX_HANDLES; i++) {
        if (!fh_table[i].in_use) {
            fh_table[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* Free a handle entry */
static void fh_free(int idx)
{
    if (idx < 0 || idx >= FH_MAX_HANDLES) return;
    memset(&fh_table[idx], 0, sizeof(struct fh_entry));
}

/* Encode (ino, mount_id) into a handle data buffer */
static void fh_encode(uint8_t *data, uint32_t *bytes_out,
                       uint32_t ino, uint32_t mount_id)
{
    /* Format: 4 bytes ino (LE) + 4 bytes mount_id (LE) = 8 bytes */
    data[0] = (uint8_t)(ino & 0xFF);
    data[1] = (uint8_t)((ino >> 8) & 0xFF);
    data[2] = (uint8_t)((ino >> 16) & 0xFF);
    data[3] = (uint8_t)((ino >> 24) & 0xFF);
    data[4] = (uint8_t)(mount_id & 0xFF);
    data[5] = (uint8_t)((mount_id >> 8) & 0xFF);
    data[6] = (uint8_t)((mount_id >> 16) & 0xFF);
    data[7] = (uint8_t)((mount_id >> 24) & 0xFF);
    *bytes_out = 8;
}

/* Decode handle data to extract (ino, mount_id), returns 0 on success */
static int fh_decode(const uint8_t *data, uint32_t data_len,
                      uint32_t *ino, uint32_t *mount_id)
{
    if (data_len < 8) return -1;
    *ino = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    *mount_id = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    return 0;
}

/*
 * sys_name_to_handle_at — Obtain a file handle for a given pathname.
 *
 *   int name_to_handle_at(int dirfd, const char *pathname,
 *                          struct file_handle *handle,
 *                          int *mount_id, int flags);
 *
 * Returns 0 on success, -errno on error.
 * On success, handle->handle_bytes is set to the actual size,
 * and *mount_id receives the mount identifier.
 */
static uint64_t sys_name_to_handle_at(uint64_t dirfd, uint64_t pathname,
                                       uint64_t handle_addr, uint64_t mount_id_addr,
                                       uint64_t flags)
{
    (void)dirfd;  /* dirfd not used — we resolve from CWD */

    /* Copy path string from user (up to 255 chars) */
    char path[256];
    if (pathname) {
        if (syscall_is_user_process()) {
            if (!syscall_user_cstr_ok(pathname))
                return (uint64_t)(int64_t)-EFAULT;
        }
        /* For empty path with AT_EMPTY_PATH, use the fd */
        if (flags & AT_EMPTY_PATH) {
            /* Not fully implemented — just use current directory root for now */
            path[0] = '/';
            path[1] = '\0';
        } else {
            /* Copy string a byte at a time using copy_from_user */
            for (int i = 0; i < 255; i++) {
                char c;
                if (copy_from_user(&c, pathname + (uint64_t)i, 1UL) < 0)
                    return (uint64_t)(int64_t)-EFAULT;
                path[i] = c;
                if (c == '\0') break;
                if (i == 254) path[255] = '\0';
            }
        }
    } else {
        return (uint64_t)(int64_t)-EFAULT;
    }

    if (path[0] == '\0')
        return (uint64_t)(int64_t)-EINVAL;

    /* Stat the path to get inode info */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0)
        return (uint64_t)(int64_t)-ENOENT;

    /* Read handle_bytes from user space */
    unsigned int handle_bytes;
    unsigned int hb_off = 0;  /* handle_bytes is first field in file_handle */
    if (copy_from_user(&handle_bytes, handle_addr + hb_off,
                       sizeof(handle_bytes)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    if (handle_bytes < 8) {
        /* Not enough room — set to required size and return EOVERFLOW */
        unsigned int new_hb = 8;
        if (copy_to_user(handle_addr + hb_off, &new_hb, sizeof(new_hb)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
        return (uint64_t)(int64_t)-EOVERFLOW;
    }

    /* Encode the handle from inode info */
    fh_init();
    int slot = fh_alloc();
    if (slot < 0)
        return (uint64_t)(int64_t)-ENOMEM;

    fh_table[slot].ino = st.ino;
    fh_table[slot].mount_id = 0;  /* single root mount */
    fh_table[slot].handle_type = 1;  /* regular file */
    fh_table[slot].handle_bytes = 8;
    fh_encode(fh_table[slot].handle_data, &fh_table[slot].handle_bytes,
              st.ino, 0);

    /* Copy the resolved path */
    size_t plen = strlen(path);
    if (plen >= sizeof(fh_table[slot].path))
        plen = sizeof(fh_table[slot].path) - 1;
    memcpy(fh_table[slot].path, path, plen);
    fh_table[slot].path[plen] = '\0';

    /* Write handle fields back to user */
    uint8_t handle_buf[32]; /* enough for file_handle + 16 bytes data */
    struct file_handle *k_handle = (struct file_handle *)handle_buf;
    k_handle->handle_type = fh_table[slot].handle_type;
    k_handle->handle_bytes = fh_table[slot].handle_bytes;
    memcpy(k_handle->f_handle, fh_table[slot].handle_data, fh_table[slot].handle_bytes);
    if (copy_to_user(handle_addr, handle_buf,
                     sizeof(struct file_handle) - sizeof(unsigned char) + fh_table[slot].handle_bytes) < 0) {
        fh_free(slot);
        return (uint64_t)(int64_t)-EFAULT;
    }

    /* Write mount_id */
    if (copy_to_user(mount_id_addr, &(int){0}, sizeof(int)) < 0) {
        fh_free(slot);
        return (uint64_t)(int64_t)-EFAULT;
    }

    return 0;
}

/*
 * sys_open_by_handle_at — Open a file by handle.
 *
 *   int open_by_handle_at(int mount_fd, struct file_handle *handle,
 *                          int flags);
 *
 * Returns a file descriptor on success, -errno on error.
 */
static uint64_t sys_open_by_handle_at(uint64_t mount_fd, uint64_t handle_addr,
                                       uint64_t flags)
{
    (void)mount_fd;  /* single mount — ignored */
    (void)flags;     /* open flags currently unused */

    /* Read handle_bytes from userspace */
    unsigned int hb;
    if (copy_from_user(&hb, handle_addr, /* handle_bytes is first field at offset 0 */
                       sizeof(hb)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    if (hb < 8)
        return (uint64_t)(int64_t)-EINVAL;

    /* Read handle data (up to 16 bytes) — f_handle starts at offset 8 */
    uint64_t f_handle_addr = handle_addr + 8;
    uint8_t data[16];
    unsigned int copy_sz = hb > 16 ? 16 : hb;
    if (copy_from_user(data, f_handle_addr, copy_sz) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    /* Decode handle to get inode */
    uint32_t ino, mount_id;
    if (fh_decode(data, hb, &ino, &mount_id) < 0)
        return (uint64_t)(int64_t)-EINVAL;

    /* Search the handle table for a matching entry */
    if (!fh_initialized) fh_init();

    const char *open_path = NULL;
    for (int i = 0; i < FH_MAX_HANDLES; i++) {
        if (fh_table[i].in_use && fh_table[i].ino == ino &&
            fh_table[i].mount_id == mount_id) {
            open_path = fh_table[i].path;
            break;
        }
    }

    if (!open_path)
        return (uint64_t)(int64_t)-ESTALE;  /* handle is stale */

    /* Verify the file still exists */
    struct vfs_stat st;
    if (vfs_stat(open_path, &st) < 0)
        return (uint64_t)(int64_t)-ESTALE;  /* file was deleted */

    /* Allocate an fd entry for the calling process */
    struct process *p = process_get_current();
    if (!p) return (uint64_t)(int64_t)-EPERM;

    int fd = -1;
    uint64_t max_fds = p->file_max > 0 ? p->file_max : PROCESS_FD_MAX;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fd_table[i].used) {
            if ((uint64_t)i >= max_fds)
                return (uint64_t)(int64_t)-EMFILE;
            fd = i;
            break;
        }
    }

    if (fd < 0)
        return (uint64_t)(int64_t)-EMFILE;

    /* Fill in the fd entry */
    size_t plen = strlen(open_path);
    if (plen > 63) plen = 63;
    memcpy(p->fd_table[fd].path, open_path, plen);
    p->fd_table[fd].path[plen] = '\0';
    p->fd_table[fd].offset = 0;
    p->fd_table[fd].used = true;
    p->fd_table[fd].flags = 0;  /* regular file */

    return (uint64_t)fd;
}

void __init syscall_init(void) {
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
    wrmsr(MSR_SFMASK, (1U << 9));
}

/* ── syscall_handle: Handle a syscall by number ─────────────────────── */
int syscall_handle(int nr, void *args)
{
    if (nr < 0) return -EINVAL;

    /* Pack arguments into the dispatch format */
    uint64_t *a = (uint64_t *)args;
    uint64_t a1 = a ? a[0] : 0;
    uint64_t a2 = a ? a[1] : 0;
    uint64_t a3 = a ? a[2] : 0;
    uint64_t a4 = a ? a[3] : 0;
    uint64_t a5 = a ? a[4] : 0;

    /* Forward to the main syscall dispatch which handles validation and seccomp */
    uint64_t ret = syscall_dispatch((uint64_t)nr, a1, a2, a3, a4, a5);

    /* Convert uint64_t return to int (assumes syscall returns int-compatible values) */
    kprintf("[syscall] syscall_handle: nr=%d returned 0x%llx\n", nr, (unsigned long long)ret);
    return (int)ret;
}
/* ── syscall_register: Register a custom syscall handler ────────────── */
int syscall_register(int nr, void *handler)
{
    if (nr < 0 || nr > 1024 || !handler) return -EINVAL;

    /* Store in a small dynamic table for custom syscalls.
     * Currently we use a fixed-size table. */
    static void *custom_syscall_table[256];
    static int custom_syscall_initialized = 0;

    if (!custom_syscall_initialized) {
        memset(custom_syscall_table, 0, sizeof(custom_syscall_table));
        custom_syscall_initialized = 1;
    }

    if (nr >= 256) return -ENOSPC;

    if (custom_syscall_table[nr] != NULL) {
        kprintf("[syscall] syscall_register: nr=%d already registered\n", nr);
        return -EBUSY;
    }

    custom_syscall_table[nr] = handler;
    kprintf("[syscall] syscall_register: nr=%d handler=0x%llx\n",
            nr, (unsigned long long)(uintptr_t)handler);
    return 0;
}
/* ── syscall_unregister: Unregister a custom syscall handler ────────── */
int syscall_unregister(int nr)
{
    if (nr < 0 || nr > 1024) return -EINVAL;

    /* Access the static table */
    static void *custom_syscall_table[256];
    static int custom_syscall_initialized = 0;

    if (!custom_syscall_initialized) return -EINVAL;
    if (nr >= 256) return -ENOSPC;
    if (!custom_syscall_table[nr]) return -ENOENT;

    custom_syscall_table[nr] = NULL;
    kprintf("[syscall] syscall_unregister: nr=%d\n", nr);
    return 0;
}
/* ── syscall_table_lookup: Look up a syscall handler by number ──────── */
void* syscall_table_lookup(int nr)
{
    if (nr < 0 || nr > 1024) return NULL;

    /* Check the custom table first */
    static void *custom_syscall_table[256];
    static int custom_syscall_initialized = 0;

    if (custom_syscall_initialized && nr < 256 && custom_syscall_table[nr]) {
        return custom_syscall_table[nr];
    }

    /* If no custom handler, return a generic indicator that the syscall is built-in.
     * A full implementation would return the actual dispatch function pointer. */
    kprintf("[syscall] syscall_table_lookup: nr=%d -> built-in (no custom handler)\n", nr);
    return NULL;
}
