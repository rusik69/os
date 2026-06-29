/*
 * syscall_linux.c — Linux x86-64 ABI syscall dispatch table
 *
 * Implements sys_call_table[], an array of function pointers indexed by
 * Linux __NR_* syscall numbers (0–334). Each entry either points to a
 * Linux-compatible wrapper that translates arguments and forwards to the
 * corresponding internal sys_* handler, or to sys_ni_syscall() which
 * returns -ENOSYS for unimplemented or stub syscalls.
 *
 * The table is referenced from syscall_asm.asm (syscall_linux_entry) and
 * from kpti_asm.asm when the Linux ABI is active.
 */

#include "syscall.h"
#include "module.h"

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Linux x86-64 ABI syscall dispatch table — sys_call_table[]");
MODULE_AUTHOR("Ruslan Gustomiasov");

/*
 * linux_syscall_t — type of a Linux-compatible syscall handler
 *
 * All Linux x86-64 syscalls receive up to 6 arguments via registers
 * (rdi, rsi, rdx, r10, r8, r9).  The handler returns a uint64_t result
 * which, for error cases, is a negative errno value (Linux convention).
 */
typedef uint64_t (*linux_syscall_t)(uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t);

/*
 * sys_ni_syscall — placeholder for unimplemented syscalls
 *
 * Returns -ENOSYS to inform the caller that this syscall number is not
 * implemented.  Logging can be added per-subsystem as needed.
 */
static uint64_t sys_ni_syscall(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    (void)a6;
    return (uint64_t)(int64_t)-ENOSYS;
}

/*
 * sys_call_table[] — Linux x86-64 syscall dispatch table
 *
 * Indexed by Linux __NR_* syscall numbers (0 through __NR_syscalls-1).
 * Each entry points to the appropriate Linux-compatible handler or to
 * sys_ni_syscall for unimplemented syscalls.
 *
 * Entries are explicitly ordered by __NR number so that assembly-level
 * dispatch via syscall_linux_entry can index directly.
 *
 * NOTE: This table is populated incrementally.  Wrappers for directly
 * mappable syscalls (where the internal SYS_* handler already exists) are
 * added as the corresponding Linux-compatible wrappers are implemented.
 * For now, all entries return -ENOSYS.
 */
__attribute__((used))
linux_syscall_t sys_call_table[__NR_syscalls] = {
    [0]  = sys_ni_syscall,  /* __NR_read */
    [1]  = sys_ni_syscall,  /* __NR_write */
    [2]  = sys_ni_syscall,  /* __NR_open */
    [3]  = sys_ni_syscall,  /* __NR_close */
    [4]  = sys_ni_syscall,  /* __NR_stat */
    [5]  = sys_ni_syscall,  /* __NR_fstat */
    [6]  = sys_ni_syscall,  /* __NR_lstat */
    [7]  = sys_ni_syscall,  /* __NR_poll */
    [8]  = sys_ni_syscall,  /* __NR_lseek */
    [9]  = sys_ni_syscall,  /* __NR_mmap */
    [10] = sys_ni_syscall,  /* __NR_mprotect */
    [11] = sys_ni_syscall,  /* __NR_munmap */
    [12] = sys_ni_syscall,  /* __NR_brk */
    [13] = sys_ni_syscall,  /* __NR_rt_sigaction */
    [14] = sys_ni_syscall,  /* __NR_rt_sigprocmask */
    [15] = sys_ni_syscall,  /* __NR_rt_sigreturn */
    [16] = sys_ni_syscall,  /* __NR_ioctl */
    [17] = sys_ni_syscall,  /* __NR_pread64 */
    [18] = sys_ni_syscall,  /* __NR_pwrite64 */
    [19] = sys_ni_syscall,  /* __NR_readv */
    [20] = sys_ni_syscall,  /* __NR_writev */
    [21] = sys_ni_syscall,  /* __NR_access */
    [22] = sys_ni_syscall,  /* __NR_pipe */
    [23] = sys_ni_syscall,  /* __NR_select */
    [24] = sys_ni_syscall,  /* __NR_sched_yield */
    [25] = sys_ni_syscall,  /* __NR_mremap */
    [26] = sys_ni_syscall,  /* __NR_msync */
    [27] = sys_ni_syscall,  /* __NR_mincore */
    [28] = sys_ni_syscall,  /* __NR_madvise */
    [29] = sys_ni_syscall,  /* __NR_shmget */
    [30] = sys_ni_syscall,  /* __NR_shmat */
    [31] = sys_ni_syscall,  /* __NR_shmctl */
    [32] = sys_ni_syscall,  /* __NR_dup */
    [33] = sys_ni_syscall,  /* __NR_dup2 */
    [34] = sys_ni_syscall,  /* __NR_pause */
    [35] = sys_ni_syscall,  /* __NR_nanosleep */
    [36] = sys_ni_syscall,  /* __NR_getitimer */
    [37] = sys_ni_syscall,  /* __NR_alarm */
    [38] = sys_ni_syscall,  /* __NR_setitimer */
    [39] = sys_ni_syscall,  /* __NR_getpid */
    [40] = sys_ni_syscall,  /* __NR_sendfile */
    [41] = sys_ni_syscall,  /* __NR_socket */
    [42] = sys_ni_syscall,  /* __NR_connect */
    [43] = sys_ni_syscall,  /* __NR_accept */
    [44] = sys_ni_syscall,  /* __NR_sendto */
    [45] = sys_ni_syscall,  /* __NR_recvfrom */
    [46] = sys_ni_syscall,  /* __NR_sendmsg */
    [47] = sys_ni_syscall,  /* __NR_recvmsg */
    [48] = sys_ni_syscall,  /* __NR_shutdown */
    [49] = sys_ni_syscall,  /* __NR_bind */
    [50] = sys_ni_syscall,  /* __NR_listen */
    [51] = sys_ni_syscall,  /* __NR_getsockname */
    [52] = sys_ni_syscall,  /* __NR_getpeername */
    [53] = sys_ni_syscall,  /* __NR_socketpair */
    [54] = sys_ni_syscall,  /* __NR_setsockopt */
    [55] = sys_ni_syscall,  /* __NR_getsockopt */
    [56] = sys_ni_syscall,  /* __NR_clone */
    [57] = sys_ni_syscall,  /* __NR_fork */
    [58] = sys_ni_syscall,  /* __NR_vfork */
    [59] = sys_ni_syscall,  /* __NR_execve */
    [60] = sys_ni_syscall,  /* __NR_exit */
    [61] = sys_ni_syscall,  /* __NR_wait4 */
    [62] = sys_ni_syscall,  /* __NR_kill */
    [63] = sys_ni_syscall,  /* __NR_uname */
    [64] = sys_ni_syscall,  /* __NR_semget */
    [65] = sys_ni_syscall,  /* __NR_semop */
    [66] = sys_ni_syscall,  /* __NR_semctl */
    [67] = sys_ni_syscall,  /* __NR_shmdt */
    [68] = sys_ni_syscall,  /* __NR_msgget */
    [69] = sys_ni_syscall,  /* __NR_msgsnd */
    [70] = sys_ni_syscall,  /* __NR_msgrcv */
    [71] = sys_ni_syscall,  /* __NR_msgctl */
    [72] = sys_ni_syscall,  /* __NR_fcntl */
    [73] = sys_ni_syscall,  /* __NR_flock */
    [74] = sys_ni_syscall,  /* __NR_fsync */
    [75] = sys_ni_syscall,  /* __NR_fdatasync */
    [76] = sys_ni_syscall,  /* __NR_truncate */
    [77] = sys_ni_syscall,  /* __NR_ftruncate */
    [78] = sys_ni_syscall,  /* __NR_getdents */
    [79] = sys_ni_syscall,  /* __NR_getcwd */
    [80] = sys_ni_syscall,  /* __NR_chdir */
    [81] = sys_ni_syscall,  /* __NR_fchdir */
    [82] = sys_ni_syscall,  /* __NR_rename */
    [83] = sys_ni_syscall,  /* __NR_mkdir */
    [84] = sys_ni_syscall,  /* __NR_rmdir */
    [85] = sys_ni_syscall,  /* __NR_creat */
    [86] = sys_ni_syscall,  /* __NR_link */
    [87] = sys_ni_syscall,  /* __NR_unlink */
    [88] = sys_ni_syscall,  /* __NR_symlink */
    [89] = sys_ni_syscall,  /* __NR_readlink */
    [90] = sys_ni_syscall,  /* __NR_chmod */
    [91] = sys_ni_syscall,  /* __NR_fchmod */
    [92] = sys_ni_syscall,  /* __NR_chown */
    [93] = sys_ni_syscall,  /* __NR_fchown */
    [94] = sys_ni_syscall,  /* __NR_lchown */
    [95] = sys_ni_syscall,  /* __NR_umask */
    [96] = sys_ni_syscall,  /* __NR_gettimeofday */
    [97] = sys_ni_syscall,  /* __NR_getrlimit */
    [98] = sys_ni_syscall,  /* __NR_getrusage */
    [99] = sys_ni_syscall,  /* __NR_sysinfo */
    [100] = sys_ni_syscall, /* __NR_times */
    [101] = sys_ni_syscall, /* __NR_ptrace */
    [102] = sys_ni_syscall, /* __NR_getuid */
    [103] = sys_ni_syscall, /* __NR_syslog */
    [104] = sys_ni_syscall, /* __NR_getgid */
    [105] = sys_ni_syscall, /* __NR_setuid */
    [106] = sys_ni_syscall, /* __NR_setgid */
    [107] = sys_ni_syscall, /* __NR_geteuid */
    [108] = sys_ni_syscall, /* __NR_getegid */
    [109] = sys_ni_syscall, /* __NR_setpgid */
    [110] = sys_ni_syscall, /* __NR_getppid */
    [111] = sys_ni_syscall, /* __NR_getpgrp */
    [112] = sys_ni_syscall, /* __NR_setsid */
    [113] = sys_ni_syscall, /* __NR_setreuid */
    [114] = sys_ni_syscall, /* __NR_setregid */
    [115] = sys_ni_syscall, /* __NR_getgroups */
    [116] = sys_ni_syscall, /* __NR_setgroups */
    [117] = sys_ni_syscall, /* __NR_setresuid */
    [118] = sys_ni_syscall, /* __NR_getresuid */
    [119] = sys_ni_syscall, /* __NR_setresgid */
    [120] = sys_ni_syscall, /* __NR_getresgid */
    [121] = sys_ni_syscall, /* __NR_getpgid */
    [122] = sys_ni_syscall, /* __NR_setfsuid */
    [123] = sys_ni_syscall, /* __NR_setfsgid */
    [124] = sys_ni_syscall, /* __NR_getsid */
    [125] = sys_ni_syscall, /* __NR_capget */
    [126] = sys_ni_syscall, /* __NR_capset */
    [127] = sys_ni_syscall, /* __NR_rt_sigpending */
    [128] = sys_ni_syscall, /* __NR_rt_sigtimedwait */
    [129] = sys_ni_syscall, /* __NR_rt_sigqueueinfo */
    [130] = sys_ni_syscall, /* __NR_rt_sigsuspend */
    [131] = sys_ni_syscall, /* __NR_sigaltstack */
    [132] = sys_ni_syscall, /* __NR_utime */
    [133] = sys_ni_syscall, /* __NR_mknod */
    [134] = sys_ni_syscall, /* __NR_uselib */
    [135] = sys_ni_syscall, /* __NR_personality */
    [136] = sys_ni_syscall, /* __NR_ustat */
    [137] = sys_ni_syscall, /* __NR_statfs */
    [138] = sys_ni_syscall, /* __NR_fstatfs */
    [139] = sys_ni_syscall, /* __NR_sysfs */
    [140] = sys_ni_syscall, /* __NR_getpriority */
    [141] = sys_ni_syscall, /* __NR_setpriority */
    [142] = sys_ni_syscall, /* __NR_sched_setparam */
    [143] = sys_ni_syscall, /* __NR_sched_getparam */
    [144] = sys_ni_syscall, /* __NR_sched_setscheduler */
    [145] = sys_ni_syscall, /* __NR_sched_getscheduler */
    [146] = sys_ni_syscall, /* __NR_sched_get_priority_max */
    [147] = sys_ni_syscall, /* __NR_sched_get_priority_min */
    [148] = sys_ni_syscall, /* __NR_sched_rr_get_interval */
    [149] = sys_ni_syscall, /* __NR_mlock */
    [150] = sys_ni_syscall, /* __NR_munlock */
    [151] = sys_ni_syscall, /* __NR_mlockall */
    [152] = sys_ni_syscall, /* __NR_munlockall */
    [153] = sys_ni_syscall, /* __NR_vhangup */
    [154] = sys_ni_syscall, /* __NR_modify_ldt */
    [155] = sys_ni_syscall, /* __NR_pivot_root */
    [156] = sys_ni_syscall, /* __NR__sysctl */
    [157] = sys_ni_syscall, /* __NR_prctl */
    [158] = sys_ni_syscall, /* __NR_arch_prctl */
    [159] = sys_ni_syscall, /* __NR_adjtimex */
    [160] = sys_ni_syscall, /* __NR_setrlimit */
    [161] = sys_ni_syscall, /* __NR_chroot */
    [162] = sys_ni_syscall, /* __NR_sync */
    [163] = sys_ni_syscall, /* __NR_acct */
    [164] = sys_ni_syscall, /* __NR_settimeofday */
    [165] = sys_ni_syscall, /* __NR_mount */
    [166] = sys_ni_syscall, /* __NR_umount2 */
    [167] = sys_ni_syscall, /* __NR_swapon */
    [168] = sys_ni_syscall, /* __NR_swapoff */
    [169] = sys_ni_syscall, /* __NR_reboot */
    [170] = sys_ni_syscall, /* __NR_sethostname */
    [171] = sys_ni_syscall, /* __NR_setdomainname */
    [172] = sys_ni_syscall, /* __NR_iopl */
    [173] = sys_ni_syscall, /* __NR_ioperm */
    [174] = sys_ni_syscall, /* __NR_create_module */
    [175] = sys_ni_syscall, /* __NR_init_module */
    [176] = sys_ni_syscall, /* __NR_delete_module */
    [177] = sys_ni_syscall, /* __NR_get_kernel_syms */
    [178] = sys_ni_syscall, /* __NR_query_module */
    [179] = sys_ni_syscall, /* __NR_quotactl */
    [180] = sys_ni_syscall, /* __NR_nfsservctl */
    [181] = sys_ni_syscall, /* __NR_getpmsg */
    [182] = sys_ni_syscall, /* __NR_putpmsg */
    [183] = sys_ni_syscall, /* __NR_afs_syscall */
    [184] = sys_ni_syscall, /* __NR_tuxcall */
    [185] = sys_ni_syscall, /* __NR_security */
    [186] = sys_ni_syscall, /* __NR_gettid */
    [187] = sys_ni_syscall, /* __NR_readahead */
    [188] = sys_ni_syscall, /* __NR_setxattr */
    [189] = sys_ni_syscall, /* __NR_lsetxattr */
    [190] = sys_ni_syscall, /* __NR_fsetxattr */
    [191] = sys_ni_syscall, /* __NR_getxattr */
    [192] = sys_ni_syscall, /* __NR_lgetxattr */
    [193] = sys_ni_syscall, /* __NR_fgetxattr */
    [194] = sys_ni_syscall, /* __NR_listxattr */
    [195] = sys_ni_syscall, /* __NR_llistxattr */
    [196] = sys_ni_syscall, /* __NR_flistxattr */
    [197] = sys_ni_syscall, /* __NR_removexattr */
    [198] = sys_ni_syscall, /* __NR_lremovexattr */
    [199] = sys_ni_syscall, /* __NR_fremovexattr */
    [200] = sys_ni_syscall, /* __NR_tkill */
    [201] = sys_ni_syscall, /* __NR_time */
    [202] = sys_ni_syscall, /* __NR_futex */
    [203] = sys_ni_syscall, /* __NR_sched_setaffinity */
    [204] = sys_ni_syscall, /* __NR_sched_getaffinity */
    [205] = sys_ni_syscall, /* __NR_set_thread_area */
    [206] = sys_ni_syscall, /* __NR_io_setup */
    [207] = sys_ni_syscall, /* __NR_io_destroy */
    [208] = sys_ni_syscall, /* __NR_io_getevents */
    [209] = sys_ni_syscall, /* __NR_io_submit */
    [210] = sys_ni_syscall, /* __NR_io_cancel */
    [211] = sys_ni_syscall, /* __NR_get_thread_area */
    [212] = sys_ni_syscall, /* __NR_lookup_dcookie */
    [213] = sys_ni_syscall, /* __NR_epoll_create */
    [214] = sys_ni_syscall, /* __NR_epoll_ctl_old */
    [215] = sys_ni_syscall, /* __NR_epoll_wait_old */
    [216] = sys_ni_syscall, /* __NR_remap_file_pages */
    [217] = sys_ni_syscall, /* __NR_getdents64 */
    [218] = sys_ni_syscall, /* __NR_set_tid_address */
    [219] = sys_ni_syscall, /* __NR_restart_syscall */
    [220] = sys_ni_syscall, /* __NR_semtimedop */
    [221] = sys_ni_syscall, /* __NR_fadvise64 */
    [222] = sys_ni_syscall, /* __NR_timer_create */
    [223] = sys_ni_syscall, /* __NR_timer_settime */
    [224] = sys_ni_syscall, /* __NR_timer_gettime */
    [225] = sys_ni_syscall, /* __NR_timer_getoverrun */
    [226] = sys_ni_syscall, /* __NR_timer_delete */
    [227] = sys_ni_syscall, /* __NR_clock_settime */
    [228] = sys_ni_syscall, /* __NR_clock_gettime */
    [229] = sys_ni_syscall, /* __NR_clock_getres */
    [230] = sys_ni_syscall, /* __NR_clock_nanosleep */
    [231] = sys_ni_syscall, /* __NR_exit_group */
    [232] = sys_ni_syscall, /* __NR_epoll_wait */
    [233] = sys_ni_syscall, /* __NR_epoll_ctl */
    [234] = sys_ni_syscall, /* __NR_tgkill */
    [235] = sys_ni_syscall, /* __NR_utimes */
    [236] = sys_ni_syscall, /* __NR_vserver */
    [237] = sys_ni_syscall, /* __NR_mbind */
    [238] = sys_ni_syscall, /* __NR_set_mempolicy */
    [239] = sys_ni_syscall, /* __NR_get_mempolicy */
    [240] = sys_ni_syscall, /* __NR_mq_open */
    [241] = sys_ni_syscall, /* __NR_mq_unlink */
    [242] = sys_ni_syscall, /* __NR_mq_timedsend */
    [243] = sys_ni_syscall, /* __NR_mq_timedreceive */
    [244] = sys_ni_syscall, /* __NR_mq_notify */
    [245] = sys_ni_syscall, /* __NR_mq_getsetattr */
    [246] = sys_ni_syscall, /* __NR_kexec_load */
    [247] = sys_ni_syscall, /* __NR_waitid */
    [248] = sys_ni_syscall, /* __NR_add_key */
    [249] = sys_ni_syscall, /* __NR_request_key */
    [250] = sys_ni_syscall, /* __NR_keyctl */
    [251] = sys_ni_syscall, /* __NR_ioprio_set */
    [252] = sys_ni_syscall, /* __NR_ioprio_get */
    [253] = sys_ni_syscall, /* __NR_inotify_init */
    [254] = sys_ni_syscall, /* __NR_inotify_add_watch */
    [255] = sys_ni_syscall, /* __NR_inotify_rm_watch */
    [256] = sys_ni_syscall, /* __NR_migrate_pages */
    [257] = sys_ni_syscall, /* __NR_openat */
    [258] = sys_ni_syscall, /* __NR_mkdirat */
    [259] = sys_ni_syscall, /* __NR_mknodat */
    [260] = sys_ni_syscall, /* __NR_fchownat */
    [261] = sys_ni_syscall, /* __NR_futimesat */
    [262] = sys_ni_syscall, /* __NR_newfstatat */
    [263] = sys_ni_syscall, /* __NR_unlinkat */
    [264] = sys_ni_syscall, /* __NR_renameat */
    [265] = sys_ni_syscall, /* __NR_linkat */
    [266] = sys_ni_syscall, /* __NR_symlinkat */
    [267] = sys_ni_syscall, /* __NR_readlinkat */
    [268] = sys_ni_syscall, /* __NR_fchmodat */
    [269] = sys_ni_syscall, /* __NR_faccessat */
    [270] = sys_ni_syscall, /* __NR_pselect6 */
    [271] = sys_ni_syscall, /* __NR_ppoll */
    [272] = sys_ni_syscall, /* __NR_unshare */
    [273] = sys_ni_syscall, /* __NR_set_robust_list */
    [274] = sys_ni_syscall, /* __NR_get_robust_list */
    [275] = sys_ni_syscall, /* __NR_splice */
    [276] = sys_ni_syscall, /* __NR_tee */
    [277] = sys_ni_syscall, /* __NR_sync_file_range */
    [278] = sys_ni_syscall, /* __NR_vmsplice */
    [279] = sys_ni_syscall, /* __NR_move_pages */
    [280] = sys_ni_syscall, /* __NR_utimensat */
    [281] = sys_ni_syscall, /* __NR_epoll_pwait */
    [282] = sys_ni_syscall, /* __NR_signalfd */
    [283] = sys_ni_syscall, /* __NR_timerfd_create */
    [284] = sys_ni_syscall, /* __NR_eventfd */
    [285] = sys_ni_syscall, /* __NR_fallocate */
    [286] = sys_ni_syscall, /* __NR_timerfd_settime */
    [287] = sys_ni_syscall, /* __NR_timerfd_gettime */
    [288] = sys_ni_syscall, /* __NR_accept4 */
    [289] = sys_ni_syscall, /* __NR_signalfd4 */
    [290] = sys_ni_syscall, /* __NR_eventfd2 */
    [291] = sys_ni_syscall, /* __NR_epoll_create1 */
    [292] = sys_ni_syscall, /* __NR_dup3 */
    [293] = sys_ni_syscall, /* __NR_pipe2 */
    [294] = sys_ni_syscall, /* __NR_inotify_init1 */
    [295] = sys_ni_syscall, /* __NR_preadv */
    [296] = sys_ni_syscall, /* __NR_pwritev */
    [297] = sys_ni_syscall, /* __NR_rt_tgsigqueueinfo */
    [298] = sys_ni_syscall, /* __NR_perf_event_open */
    [299] = sys_ni_syscall, /* __NR_recvmmsg */
    [300] = sys_ni_syscall, /* __NR_fanotify_init */
    [301] = sys_ni_syscall, /* __NR_fanotify_mark */
    [302] = sys_ni_syscall, /* __NR_prlimit64 */
    [303] = sys_ni_syscall, /* __NR_name_to_handle_at */
    [304] = sys_ni_syscall, /* __NR_open_by_handle_at */
    [305] = sys_ni_syscall, /* __NR_clock_adjtime */
    [306] = sys_ni_syscall, /* __NR_syncfs */
    [307] = sys_ni_syscall, /* __NR_sendmmsg */
    [308] = sys_ni_syscall, /* __NR_setns */
    [309] = sys_ni_syscall, /* __NR_getcpu */
    [310] = sys_ni_syscall, /* __NR_process_vm_readv */
    [311] = sys_ni_syscall, /* __NR_process_vm_writev */
    [312] = sys_ni_syscall, /* __NR_kcmp */
    [313] = sys_ni_syscall, /* __NR_finit_module */
    [314] = sys_ni_syscall, /* __NR_sched_setattr */
    [315] = sys_ni_syscall, /* __NR_sched_getattr */
    [316] = sys_ni_syscall, /* __NR_renameat2 */
    [317] = sys_ni_syscall, /* __NR_seccomp */
    [318] = sys_ni_syscall, /* __NR_getrandom */
    [319] = sys_ni_syscall, /* __NR_memfd_create */
    [320] = sys_ni_syscall, /* __NR_kexec_file_load */
    [321] = sys_ni_syscall, /* __NR_bpf */
    [322] = sys_ni_syscall, /* __NR_execveat */
    [323] = sys_ni_syscall, /* __NR_userfaultfd */
    [324] = sys_ni_syscall, /* __NR_membarrier */
    [325] = sys_ni_syscall, /* __NR_mlock2 */
    [326] = sys_ni_syscall, /* __NR_copy_file_range */
    [327] = sys_ni_syscall, /* __NR_preadv2 */
    [328] = sys_ni_syscall, /* __NR_pwritev2 */
    [329] = sys_ni_syscall, /* __NR_pkey_mprotect */
    [330] = sys_ni_syscall, /* __NR_pkey_alloc */
    [331] = sys_ni_syscall, /* __NR_pkey_free */
    [332] = sys_ni_syscall, /* __NR_statx */
    [333] = sys_ni_syscall, /* __NR_io_pgetevents */
    [334] = sys_ni_syscall, /* __NR_rseq */
};
