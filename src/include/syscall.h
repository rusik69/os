#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* Syscall numbers (Linux-compatible ABI) */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_EXIT    4
#define SYS_GETPID  5
#define SYS_KILL    6
#define SYS_BRK     7
#define SYS_STAT    8
#define SYS_MKDIR   9
#define SYS_UNLINK  10
#define SYS_TIME    11
#define SYS_YIELD   12
#define SYS_UPTIME  13

/* Extended syscall set used by command-side libc wrappers. */
#define SYS_FS_FORMAT     100
#define SYS_FS_CREATE     101
#define SYS_FS_WRITE      102
#define SYS_FS_READ       103
#define SYS_FS_DELETE     104
#define SYS_FS_LIST       105
#define SYS_FS_STAT       106
#define SYS_FS_STAT_EX    107
#define SYS_FS_CHMOD      108
#define SYS_FS_CHOWN      109
#define SYS_FS_GET_USAGE  110
#define SYS_FS_LIST_NAMES 111
#define SYS_ATA_PRESENT   112
#define SYS_VFS_READ      113
#define SYS_VFS_WRITE     114
#define SYS_VFS_STAT      115
#define SYS_VFS_CREATE    116
#define SYS_VFS_UNLINK    117
#define SYS_VFS_READDIR   118
#define SYS_WAITPID       119
#define SYS_SLEEP_TICKS   120
#define SYS_ATA_SECTORS   121
#define SYS_AHCI_PRESENT  122
#define SYS_AHCI_SECTORS  123
#define SYS_NET_PRESENT   124
#define SYS_NET_GET_MAC   125
#define SYS_NET_GET_IP    126
#define SYS_NET_GET_GW    127
/* getdents64 syscall (Item 453) */
#define SYS_GETDENTS64    78
#define SYS_NET_GET_MASK  128
#define SYS_NET_DNS       129
#define SYS_NET_PING      130
#define SYS_NET_UDP_SEND  131
#define SYS_NET_HTTP_GET  132
#define SYS_NET_ARP_LIST  133
#define SYS_PROC_LIST     134
#define SYS_PCI_LIST      135
#define SYS_USB_LIST      136
#define SYS_HWINFO_PRINT  137

/* User/Session management syscalls (Phase 3 Group 1) */
#define SYS_USER_FIND     138
#define SYS_USER_ADD      139
#define SYS_USER_DELETE   140
#define SYS_USER_PASSWD   141
#define SYS_SESSION_LOGIN  142
#define SYS_SESSION_LOGOUT 143
#define SYS_SESSION_GET   144
#define SYS_USERS_COUNT   145
#define SYS_USERS_GET_BY_INDEX 146

/* Hardware/Audio syscalls (Phase 3 Group 2) */
#define SYS_SPEAKER_BEEP  147
#define SYS_RTC_GET_TIME  148
#define SYS_ACPI_SHUTDOWN 149

/* I/O and Memory syscalls (Phase 3 Group 3a) */
#define SYS_MOUSE_GET_STATE 150
#define SYS_SERIAL_READ     151
#define SYS_SERIAL_WRITE    152
#define SYS_CMOS_READ_BYTE  153
#define SYS_PMM_GET_STATS   154

/* Specialized syscalls (Phase 3 Group 3b) */
#define SYS_ELF_EXEC        155
#define SYS_SCRIPT_EXEC     156
#define SYS_FAT_MOUNT       157
#define SYS_FAT_IS_MOUNTED  158
#define SYS_FAT_LIST_DIR    159
#define SYS_FAT_READ_FILE   160
#define SYS_FAT_FILE_SIZE   161

/* Shell-core syscalls (Phase 3 Group 3b, shell linkage slice) */
#define SYS_SHELL_HISTORY_SHOW 162
#define SYS_SHELL_READ_LINE    163
#define SYS_SHELL_VAR_SET      164
#define SYS_SHELL_EXEC_CMD     165

/* Display syscalls (Phase 3 Group 3b, color/fbinfo slice) */
#define SYS_VGA_SET_COLOR      166
#define SYS_VGA_GET_FB_INFO    167

/* Compiler syscall (Phase 3 Group 3b, cmd_cc slice) */
#define SYS_CC_COMPILE         168

/* Tmux isolation syscalls (Phase 3 Group 3b, cmd_tmux slice) */
#define SYS_KEYBOARD_GETCHAR    169
#define SYS_SHELL_HISTORY_ADD   170
#define SYS_SHELL_HISTORY_COUNT 171
#define SYS_SHELL_HISTORY_ENTRY 172
#define SYS_SHELL_TAB_COMPLETE  173
#define SYS_VGA_PUT_ENTRY_AT    174
#define SYS_VGA_SET_CURSOR      175
#define SYS_VGA_CLEAR           176
#define SYS_GUI_SHELL_RUN       177
#define SYS_PROC_SET_CAP_PROFILE 178

/* Heap syscalls — userspace malloc/free */
#define SYS_MALLOC              179
#define SYS_FREE                180
#define SYS_REALLOC             181
#define SYS_CALLOC              182

/* TCP server syscalls — userspace can act as a TCP server */
#define SYS_NET_TCP_LISTEN      183  /* listen on port (no callbacks) */
#define SYS_NET_TCP_ACCEPT      184  /* blocking accept → conn_id or -1 */
#define SYS_NET_TCP_SEND_CONN   185  /* send data on conn_id */
#define SYS_NET_TCP_RECV_CONN   186  /* recv data from conn_id */
#define SYS_NET_TCP_CLOSE_CONN  187  /* close conn_id */
#define SYS_NET_TCP_UNLISTEN    188  /* stop listening on port */
#define SYS_NET_TCP_CONNECT     189  /* outbound TCP connect → conn_id or -1 */

/* Mutex syscalls */
#define SYS_MUTEX_INIT          190  /* allocate mutex → id or -1 */
#define SYS_MUTEX_LOCK          191  /* lock(id) */
#define SYS_MUTEX_UNLOCK        192  /* unlock(id) */
#define SYS_MUTEX_DESTROY       193  /* free(id) */

/* Semaphore syscalls */
#define SYS_SEM_INIT            194  /* allocate sem(count) → id or -1 */
#define SYS_SEM_WAIT            195  /* wait/decrement */
#define SYS_SEM_POST            196  /* post/increment */
#define SYS_SEM_DESTROY         197  /* free */

/* UDP server syscalls */
#define SYS_NET_UDP_LISTEN   198  /* listen on port → 0 or -1 */
#define SYS_NET_UDP_RECV     199  /* recv pkt → bytes or -1/0 */
#define SYS_NET_UDP_UNLISTEN 200  /* stop listening */

/* Filesystem extended syscalls */
#define SYS_FS_SYMLINK  201  /* create symlink */
#define SYS_FS_READLINK 202  /* read symlink target */
#define SYS_FS_LSTAT    203  /* stat without following symlinks */

/* Working directory */
#define SYS_CHDIR        204  /* change cwd → 0 or -1 */
#define SYS_GETCWD       205  /* copy cwd into buffer → 0 or -1 */
#define SYS_SETPRIORITY  206  /* setpriority(which, who, prio) — POSIX nice */

/* Shared memory (IPC) */
#define SYS_SHM_GET   207  /* get/create segment by key → id */
#define SYS_SHM_AT    208  /* map segment → virt addr */
#define SYS_SHM_DT    209  /* decrement ref count */
#define SYS_SHM_FREE  210  /* free segment */

/* Process fork */
#define SYS_FORK      211  /* fork current process */

/* Network connection list */
#define SYS_NET_CONNLIST  212  /* fill buffer with tcp connection info */

/* Signal handling */
#define SYS_SIGNAL        213  /* register signal handler for current process */

/* File seek/truncate */
#define SYS_LSEEK         214  /* seek within open file (returns new offset) */
#define SYS_TRUNCATE      215  /* truncate file to given length */

/* Raw network send */
#define SYS_RAW_SEND      216  /* send raw Ethernet frame */

/* FD-based read/write (uses open file descriptor offset) */
#define SYS_FD_READ       217  /* read count bytes from fd at current offset */
#define SYS_FD_WRITE      218  /* write count bytes to fd at current offset */

/* Job control / priority management */
#define SYS_SETPRIORITY_PID 219  /* set target process priority */
#define SYS_GETPRIORITY     220  /* get target process priority */
#define SYS_SETPGID         221  /* set process group ID */
#define SYS_GETPGID         222  /* get process group ID */
#define SYS_KILLPG          223  /* send signal to process group */
#define SYS_AC97_PRESENT    224  /* returns 1 if AC97 available */
#define SYS_AC97_BEEP       225  /* play square-wave PCM (freq_hz, duration_ms) */
#define SYS_FAT_WRITE_FILE  226  /* write/create file on mounted FAT volume */
#define SYS_FAT_SYNC        227  /* flush FAT tables to disk */
#define SYS_DOOM_RUN        228  /* start raycast doom game (blocking) */
#define SYS_CC_COMPILE_OBJ  229  /* compile C source to relocatable .o file */
#define SYS_CC_LINK          230  /* link multiple .o files into executable */

/* Memory mapping syscalls */
#define SYS_MREMAP           370

/* Readahead: prefetch file data into page cache */
#define SYS_READAHEAD        371
#define SYS_MMAP            235  /* (addr, length, prot) → addr or -1 */
#define SYS_MUNMAP          236  /* (addr, length) → 0 or -1 */
#define SYS_MPROTECT        237  /* (addr, length, prot) → 0 or -1 */

/* CPU affinity */
#define SYS_SCHED_SETAFFINITY 238
#define SYS_SCHED_GETAFFINITY 239

/* File descriptor manipulation */
#define SYS_DUP              240  /* dup(old_fd) → new_fd or -1 */
#define SYS_DUP2             241  /* dup2(old_fd, new_fd) → new_fd or -1 */
#define SYS_FCNTL            242  /* fcntl(fd, cmd, arg) → varies */

/* I/O multiplexing */
#define SYS_SELECT           243  /* select(nfds, readfds, writefds, exceptfds, timeout) */

/* Per-process timers */
#define SYS_SETITIMER        244  /* setitimer(which, new_val, old_val) */
#define SYS_GETITIMER        245  /* getitimer(which, cur_val) */

/* High-resolution sleep */
#define SYS_NANOSLEEP        246  /* nanosleep(req, rem) */

/* System configuration */
#define SYS_SYSCONF          247  /* sysconf(name) → value or -1 */
#define SYS_UNAME            248  /* uname(struct utsname *) → 0 or -1 */

/* Filesystem / directory operations */
#define SYS_PIPE             249  /* pipe(int fds[2]) → 0 or -1 */
#define SYS_GETPPID          250  /* getppid() → parent PID */
#define SYS_ALARM            251  /* alarm(seconds) → previous alarm */
#define SYS_PAUSE            252  /* pause() → -1 (always interrupted) */
#define SYS_ACCESS           253  /* access(path, mode) → 0 or -1 */
#define SYS_GETUID           254  /* getuid() → uid */
#define SYS_GETEUID          255  /* geteuid() → euid */
#define SYS_GETGID           256  /* getgid() → gid */
#define SYS_GETEGID          257  /* getegid() → egid */
#define SYS_RMDIR            258  /* rmdir(path) → 0 or -1 */
#define SYS_RENAME           259  /* rename(old, new) → 0 or -1 */
#define SYS_CHMOD            260  /* chmod(path, mode) → 0 or -1 */
#define SYS_FSYNC            261  /* fsync(fd) → 0 or -1 */
#define SYS_SIGPROCMASK      262  /* sigprocmask(how, set, oldset) → 0 or -1 */
#define SYS_SIGPENDING       263  /* sigpending(set) → 0 or -1 */
#define SYS_READV            264  /* readv(fd, iov, iovcnt) → bytes or -1 */
#define SYS_WRITEV           265  /* writev(fd, iov, iovcnt) → bytes or -1 */
#define SYS_GETRANDOM        266  /* getrandom(buf, count, flags) → bytes or -1 */
#define SYS_REBOOT           267  /* reboot() → never returns */
#define SYS_SETHOSTNAME      268  /* sethostname(name, len) → 0 or -1 */
#define SYS_GETHOSTNAME      269  /* gethostname(name, len) → 0 or -1 */
#define SYS_UMASK            270  /* umask(mask) → old mask */
#define SYS_MKNOD            271  /* mknod(path, mode, dev) → 0 or -1 */

/* ── Production-ready improvements ──────────────────────────── */
#define SYS_PRLIMIT64         272  /* prlimit64(pid, resource, new, old) → 0 or -1 */
#define SYS_FUTEX             273  /* futex(uaddr, op, val, timeout, uaddr2, val3) → varies */
#define SYS_ARCH_PRCTL        274  /* arch_prctl(code, addr) → 0 or -1 */
#define SYS_POLL              275  /* poll(fds, nfds, timeout_ms) → count or -1 */
#define SYS_EVENTFD           276  /* eventfd(initval, flags) → fd or -1 */
#define SYS_SENDFILE          277  /* sendfile(out_fd, in_fd, offset, count) → bytes or -1 */
#define SYS_IOCTL             278  /* ioctl(fd, cmd, arg) → 0 or -1 */
#define SYS_SYSLOG            279  /* syslog(opt, buf, len) → varies */
#define SYS_PRCTL             280  /* prctl(op, a2, a3, a4, a5) → 0 or -1 */
#define SYS_SET_ROBUST_LIST   573  /* set_robust_list(head, len) → 0 or -1 */
#define SYS_GET_ROBUST_LIST   574  /* get_robust_list(pid, head_ptr, len_ptr) → 0 or -1 */
#define SYS_SHMCTL            575  /* shmctl(id, cmd, arg) → 0 or -1 */
#define SYS_MOUNT             281  /* mount(src, target, fstype, flags, data) → 0 or -1 */
#define SYS_UMOUNT            282  /* umount(target) → 0 or -1 */
#define SYS_FTRUNCATE         283  /* ftruncate(fd, length) → 0 or -1 */
#define SYS_READDIR           284  /* readdir(fd, buf, count) → bytes or 0 */
#define SYS_EXECVEAT          285  /* execveat(dirfd, path, argv, envp, flags) → 0 or -1 */
#define SYS_IOPRIO_SET         555  /* ioprio_set(which, who, ioprio) → 0 or -1 */
#define SYS_IOPRIO_GET         556  /* ioprio_get(which, who) → ioprio or -1 */
#define SYS_SCHED_SETSCHEDULER 286 /* sched_setscheduler(pid, policy, param) → 0 or -1 */
#define SYS_SCHED_GETSCHEDULER 287 /* sched_getscheduler(pid) → policy or -1 */

/* ── *at syscall family (musl/glibc requirement) ────────────── */
#define SYS_OPENAT            288  /* openat(dirfd, path, flags, mode) */
#define SYS_MKDIRAT           289  /* mkdirat(dirfd, path, mode) */
#define SYS_FSTATAT           290  /* fstatat(dirfd, path, buf, flags) */
#define SYS_UNLINKAT          291  /* unlinkat(dirfd, path, flags) */
#define SYS_RENAMEAT          292  /* renameat(olddirfd, oldpath, newdirfd, newpath) */
#define SYS_SYMLINKAT         293  /* symlinkat(target, newdirfd, linkpath) */
#define SYS_READLINKAT        294  /* readlinkat(dirfd, path, buf, bufsize) */

/* FD-oriented data synchronization */
#define SYS_FDATASYNC         572  /* fdatasync(fd) → 0 or -1 */

/* ── Memory management ──────────────────────────────────────── */
#define SYS_MLOCK             296  /* mlock(addr, len) → 0 or -1 */
#define SYS_MLOCKALL          297  /* mlockall(flags) → 0 or -1 */
#define SYS_MUNLOCK           298  /* munlock(addr, len) → 0 or -1 */
#define SYS_MUNLOCKALL        299  /* munlockall() → 0 or -1 */
#define SYS_MINCORE           300  /* mincore(addr, len, vec) → 0 or -1 */
#define SYS_MADVISE           301  /* madvise(addr, len, advice) → 0 or -1 */
#define SYS_FALLOCATE         302  /* fallocate(fd, mode, offset, len) → 0 or -1 */

/* ── Event/timer file descriptors ───────────────────────────── */
#define SYS_TIMERFD_CREATE    303  /* timerfd_create(clockid, flags) → fd or -1 */
#define SYS_TIMERFD_SETTIME   304  /* timerfd_settime(fd, flags, new, old) → 0 or -1 */
#define SYS_TIMERFD_GETTIME   305  /* timerfd_gettime(fd, cur) → 0 or -1 */
#define SYS_SIGNALFD          306  /* signalfd(fd, mask, flags) → fd or -1 */

/* ── I/O and data transfer ──────────────────────────────────── */
#define SYS_SPLICE            307  /* splice(fd_in, off_in, fd_out, off_out, len, flags) */
#define SYS_TEE               308  /* tee(fd_in, fd_out, len, flags) */
#define SYS_SENDMMSG          309  /* sendmmsg(sockfd, msgvec, vlen, flags) */
#define SYS_RECVMMSG          310  /* recvmmsg(sockfd, msgvec, vlen, flags, timeout) */
#define SYS_SYNC              311  /* sync() → void */
#define SYS_SYNCFS            312  /* syncfs(fd) → 0 or -1 */

/* ── Process and session management ─────────────────────────── */
#define SYS_SETSID            313  /* setsid() → sid or -1 */
#define SYS_GETSID            314  /* getsid(pid) → sid or -1 */
#define SYS_SIGALTSTACK       315  /* sigaltstack(ss, old_ss) → 0 or -1 */
#define SYS_PERSONALITY       316  /* personality(persona) → old persona */

/* ── BSD Socket API ─────────────────────────────────────────── */
#define SYS_SOCKET            317  /* socket(domain, type, protocol) → fd */
#define SYS_BIND              318  /* bind(sockfd, addr, addrlen) */
#define SYS_LISTEN            319  /* listen(sockfd, backlog) */
#define SYS_ACCEPT            320  /* accept(sockfd, addr, addrlen) → fd */
#define SYS_CONNECT           321  /* connect(sockfd, addr, addrlen) */
#define SYS_SETSOCKOPT        322  /* setsockopt(sockfd, level, optname, optval, optlen) */
#define SYS_GETSOCKOPT        323  /* getsockopt(sockfd, level, optname, optval, optlen) */
#define SYS_SENDMSG           324  /* sendmsg(sockfd, msg, flags) */
#define SYS_RECVMSG           325  /* recvmsg(sockfd, msg, flags) */
#define SYS_GETSOCKNAME       326  /* getsockname(sockfd, addr, addrlen) */
#define SYS_GETPEERNAME       327  /* getpeername(sockfd, addr, addrlen) */
#define SYS_SOCKETPAIR        328  /* socketpair(domain, type, protocol, sv) */

/* ── epoll ──────────────────────────────────────────────────── */
#define SYS_EPOLL_CREATE1     329  /* epoll_create1(flags) → fd */
#define SYS_EPOLL_CTL         330  /* epoll_ctl(epfd, op, fd, event) */
#define SYS_EPOLL_WAIT        331  /* epoll_wait(epfd, events, maxevents, timeout) */
#define SYS_EPOLL_PWAIT       332  /* epoll_pwait(epfd, events, maxevents, timeout, sigmask) */

/* ── POSIX Clocks & Timers ──────────────────────────────────── */
#define SYS_CLOCK_GETTIME     333  /* clock_gettime(clockid, tp) */
#define SYS_CLOCK_SETTIME     334  /* clock_settime(clockid, tp) */
#define SYS_CLOCK_GETRES      335  /* clock_getres(clockid, res) */
#define SYS_TIMER_CREATE      336  /* timer_create(clockid, sevp, timerid) */
#define SYS_TIMER_SETTIME     337  /* timer_settime(timerid, flags, new, old) */
#define SYS_TIMER_GETTIME     338  /* timer_gettime(timerid, cur) */
#define SYS_TIMER_GETOVERRUN  339  /* timer_getoverrun(timerid) → overrun count */
#define SYS_TIMER_DELETE      340  /* timer_delete(timerid) */

/* ── Modern FD operations ───────────────────────────────────── */
#define SYS_DUP3              341  /* dup3(oldfd, newfd, flags) */
#define SYS_PIPE2             342  /* pipe2(fds, flags) */
#define SYS_MKDTEMP           343  /* mkdtemp(template) → path or NULL */
#define SYS_UTIMENSAT         344  /* utimensat(dirfd, path, times, flags) */
#define SYS_FUTIMENS          345  /* futimens(fd, times) */

/* ── Filesystem & System Info ───────────────────────────────── */
#define SYS_STATFS            346  /* statfs(path, buf) */
#define SYS_FSTATFS           347  /* fstatfs(fd, buf) */
#define SYS_GETRUSAGE         348  /* getrusage(who, usage) */
#define SYS_SYSINFO           349  /* sysinfo(info) */

/* ── Inotify file monitoring (Item 340) ──────────────────────── */
#define SYS_INOTIFY_INIT1     381  /* inotify_init1(flags) → fd or -1 */
#define SYS_INOTIFY_ADD_WATCH 382  /* inotify_add_watch(fd, path, mask) → wd or -1 */
#define SYS_INOTIFY_RM_WATCH  383  /* inotify_rm_watch(fd, wd) → 0 or -1 */

/* ── Process Credentials & Scheduling ───────────────────────── */
#define SYS_GETRESUID         350  /* getresuid(ruid, euid, suid) */
#define SYS_SETRESUID         351  /* setresuid(ruid, euid, suid) */
#define SYS_GETRESGID         352  /* getresgid(rgid, egid, sgid) */
#define SYS_SETRESGID         353  /* setresgid(rgid, egid, sgid) */
#define SYS_SCHED_GETPARAM    354  /* sched_getparam(pid, param) */
#define SYS_SCHED_SETPARAM    355  /* sched_setparam(pid, param) */

/* ── POSIX IPC (Message Queues) ─────────────────────────────── */
#define SYS_MQ_OPEN           356  /* mq_open(name, oflag, mode, attr) → mqd */
#define SYS_MQ_SEND           357  /* mq_send(mqd, msg, len, prio) */
#define SYS_MQ_RECEIVE        358  /* mq_receive(mqd, msg, len, prio) */
#define SYS_MQ_UNLINK         359  /* mq_unlink(name) */

/* Get CPU info */
#define SYS_GETCPU            360  /* getcpu(cpu, node, tcache) → 0 */
#define SYS_PREADV            361  /* preadv(fd, iov, iovcnt, offset) */
#define SYS_PWRITEV           362  /* pwritev(fd, iov, iovcnt, offset) */

/* memfd_create — anonymous file descriptor with sealing support */
#define SYS_MEMFD_CREATE      365  /* memfd_create(name, flags) → fd or -1 */

/* posix_fadvise — advise file access pattern */
#define SYS_FADVISE64         372  /* fadvise64(fd, offset, len, advice) → 0 or -1 */

/* Synchronous signal acceptance */
#define SYS_SIGWAITINFO       363  /* sigwaitinfo(set, info) → signum or -1 */
#define SYS_SIGTIMEDWAIT      364  /* sigtimedwait(set, info, timeout) → signum or -1 */

/* ── Module syscalls (M17-M20) ────────────────────────────────── */
#define SYS_INIT_MODULE       366  /* init_module(path, params) → module_id or -errno */
#define SYS_FINIT_MODULE      367  /* finit_module(fd, params, flags) → module_id or -errno */
#define SYS_DELETE_MODULE     368  /* delete_module(name, flags) → 0 or -errno */
#define SYS_QUERY_MODULE      369  /* query_module(name, info_buf, buf_size) → 0 or -errno */

/* membarrier — memory barrier on all threads (used by JIT compilers, runtimes) */
#define SYS_MEMBARRIER         373  /* membarrier(cmd, flags, cpu_id) → 0 or -1 */
#define SYS_PIVOT_ROOT         374  /* pivot_root(new_root, put_old) → 0 or -1 */

/* ── Chroot (Item 117) ─────────────────────────────────────────── */
#define SYS_CHROOT             377  /* chroot(path) → 0 or -1 */

/* Zero-copy file-to-file data transfer (Item 249) */
#define SYS_COPY_FILE_RANGE     378 /* copy_file_range(fd_in, off_in, fd_out, off_out, len, flags) */

/* File handle operations (Item 250) */
#define SYS_NAME_TO_HANDLE_AT   379 /* name_to_handle_at(dirfd, pathname, handle, mount_id, flags) */
#define SYS_OPEN_BY_HANDLE_AT   380 /* open_by_handle_at(mount_fd, handle, flags) */

/* userfaultfd — Linux-compatible syscall number 323, using 384 in this kernel */
#define SYS_USERFAULTFD         384 /* userfaultfd(cmd, arg) */

/* Maximum size of a file handle (in bytes) */
#define MAX_HANDLE_SZ 128

/* AT_ flags for name_to_handle_at */
#define AT_EMPTY_PATH   0x1000   /* Allow empty path (fd refers to file directly) */

/* struct file_handle — userspace-visible file handle (Linux-compatible)
 * Defined in libc.h for userspace; kernel includes libc.h via syscall.h chain. */
#ifndef __FILE_HANDLE_DEFINED
#define __FILE_HANDLE_DEFINED
struct file_handle {
    unsigned int handle_bytes;   /* size of f_handle[] */
    int          handle_type;    /* handle type identifier */
    unsigned char f_handle[0];   /* variable-length handle data */
};
#endif

/* membarrier command codes */
#define MEMBARRIER_CMD_QUERY                    0
#define MEMBARRIER_CMD_GLOBAL                   1
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED         4
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED  8
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED        16
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED 32

/* membarrier flags */
#define MEMBARRIER_CMD_FLAG_CPU     (1 << 0)

/* ── Constants for *at syscalls ─────────────────────────────── */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW  0x100
#define AT_REMOVEDIR         0x200

/* mlockall flags */
#define MCL_CURRENT    1
#define MCL_FUTURE     2
#define MCL_ONFAULT    4

/* madvise advice values */
#define MADV_NORMAL      0
#define MADV_RANDOM      1
#define MADV_SEQUENTIAL  2
#define MADV_WILLNEED    3
#define MADV_DONTNEED    4
#define MADV_FREE        8
#define MADV_COLD        20   /* proactive reclaim hint */
#define MADV_PAGEOUT     21   /* proactive swap out */
#define MADV_REMOVE      9
#define MADV_MERGEABLE   12
#define MADV_UNMERGEABLE 13

/* posix_fadvise advice values (Linux-compatible) */
#define POSIX_FADV_NORMAL      0  /* no special treatment */
#define POSIX_FADV_RANDOM      1  /* expected random access */
#define POSIX_FADV_SEQUENTIAL  2  /* expected sequential access */
#define POSIX_FADV_WILLNEED    3  /* will need in near future */
#define POSIX_FADV_DONTNEED    4  /* not needed in near future */
#define POSIX_FADV_NOREUSE     5  /* will be accessed only once */

/* fallocate mode flags */
#define FALLOC_FL_KEEP_SIZE     1
#define FALLOC_FL_PUNCH_HOLE    2

/* signalfd flags */
#define SFD_CLOEXEC 02000000
#define SFD_NONBLOCK 04000

/* timerfd flags */
#define TFD_CLOEXEC 02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME (1 << 0)

/* timerfd clock sources */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* struct itimerspec for timerfd_settime */
struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

/* ── epoll structures ────────────────────────────────────────── */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

enum EPOLL_EVENTS {
    EPOLLIN  = 0x001,
    EPOLLOUT = 0x004,
    EPOLLERR = 0x008,
    EPOLLHUP = 0x010,
    EPOLLET  = 0x80000000,
};

struct epoll_event {
    uint32_t events;   /* Epoll events */
    uint64_t data;     /* User data variable */
};

/* ── flock structure (file locking) ──────────────────────────── */
struct flock {
    int16_t l_type;    /* F_RDLCK=0, F_WRLCK=1, F_UNLCK=2 */
    int16_t l_whence;  /* SEEK_SET=0, SEEK_CUR=1, SEEK_END=2 */
    int64_t l_start;
    int64_t l_len;     /* 0 = to EOF */
    int32_t l_pid;
};
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* ── statfs structure ────────────────────────────────────────── */
#define STATFS_MAX 64

struct statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_fsid;
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
};

/* ── sysinfo structure ───────────────────────────────────────── */
struct sysinfo {
    uint64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t totalhigh;
    uint16_t freehigh;
    uint32_t mem_unit;
};

/* ── rusage structure ────────────────────────────────────────── */
#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN -1
#define RUSAGE_THREAD   1

struct rusage {
    struct timeval ru_utime;    /* user time used */
    struct timeval ru_stime;    /* system time used */
    uint64_t ru_maxrss;         /* max resident set size */
    uint64_t ru_ixrss;          /* integral shared text memory size */
    uint64_t ru_idrss;          /* integral unshared data size */
    uint64_t ru_isrss;          /* integral unshared stack size */
    uint64_t ru_minflt;         /* page reclaims */
    uint64_t ru_majflt;         /* page faults */
    uint64_t ru_nswap;          /* swaps */
    uint64_t ru_inblock;        /* block input operations */
    uint64_t ru_oublock;        /* block output operations */
    uint64_t ru_msgsnd;         /* IPC messages sent */
    uint64_t ru_msgrcv;         /* IPC messages received */
    uint64_t ru_nsignals;       /* signals received */
    uint64_t ru_nvcsw;          /* voluntary context switches */
    uint64_t ru_nivcsw;         /* involuntary context switches */
};

/* ── POSIX timer signal event ───────────────────────────────── */
#ifndef SIGEVENT_DEFINED
#define SIGEVENT_DEFINED
struct sigevent {
    int      sigev_notify;    /* notification type */
    int      sigev_signo;     /* signal number */
    union {
        int   sigev_notify_function; /* function address (SIGEV_THREAD) */
        int   sigev_notify_attributes;
    };
};
#endif

#define SIGEV_SIGNAL   0
#define SIGEV_NONE     1
#define SIGEV_THREAD   2

/* POSIX timer ID type */
typedef int timer_t;

/* ── POSIX message queue structures ──────────────────────────── */
#ifndef MQ_ATTR_DEFINED
#define MQ_ATTR_DEFINED
struct mq_attr {
    uint64_t mq_flags;
    uint64_t mq_maxmsg;
    uint64_t mq_msgsize;
    uint64_t mq_curmsgs;
};
#endif

/* rlimit resources (Linux-compatible) */
#define RLIMIT_AS          0   /* Address space limit (bytes) */
#define RLIMIT_CORE        1   /* Core file size (bytes) */
#define RLIMIT_CPU         2   /* CPU time (seconds) */
#define RLIMIT_DATA        3   /* Data segment size (bytes) */
#define RLIMIT_FSIZE       4   /* File size (bytes) */
#define RLIMIT_NOFILE      5   /* Number of open files */
#define RLIMIT_STACK       6   /* Stack size (bytes) */
#define RLIMIT_NPROC       7   /* Number of processes */
#define RLIMIT_MEMLOCK     8   /* Locked memory (bytes) */
#define RLIMIT_LOCKS       9   /* Number of file locks */
#define RLIMIT_SIGPENDING  10  /* Number of pending signals */
#define RLIMIT_MSGQUEUE    11  /* POSIX message queue size */
#define RLIMIT_NICE        12  /* Nice value */
#define RLIMIT_RTPRIO      13  /* Real-time priority */
#define RLIMIT_NLIMITS     14
#define RLIM_INFINITY      (~0ULL)

/* Futex operations */
#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_REQUEUE      3
#define FUTEX_CMP_REQUEUE  4
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT  | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE  | FUTEX_PRIVATE_FLAG)

/* arch_prctl codes */
#define ARCH_SET_FS        0x1002
#define ARCH_GET_FS        0x1003
#define ARCH_SET_GS        0x1004
#define ARCH_GET_GS        0x1005

/* Scheduling policies */
#define SCHED_OTHER        0
#define SCHED_FIFO         1
#define SCHED_RR           2
#define SCHED_BATCH        3
#define SCHED_DEADLINE     4
#define SCHED_IDLE         5

/* Priority which values for setpriority/getpriority */
#define PRIO_PROCESS        0
#define PRIO_PGRP           1
#define PRIO_USER           2

/* Range for nice values */
#define NICE_MIN          (-20)
#define NICE_MAX            19
#define NICE_DEFAULT         0

/* struct sched_param */
struct sched_param {
    int sched_priority;
};

/* struct pollfd for poll() */
#define POLLIN     0x001
#define POLLPRI    0x002
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLNVAL   0x020

struct pollfd {
    int   fd;
    int16_t events;
    int16_t revents;
};

/* struct rlimit64 for prlimit64 */
struct rlimit64 {
    uint64_t rlim_cur;  /* soft limit */
    uint64_t rlim_max;  /* hard limit */
};

/* struct linux_dirent64 for readdir */
struct linux_dirent64 {
    uint64_t  d_ino;
    int64_t   d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char      d_name[];
};

#define DT_UNKNOWN  0
#define DT_REG      1
#define DT_DIR      2
#define DT_LNK      3

/* syslog operations (Linux-compatible) */
#define SYSLOG_ACTION_CLOSE      0
#define SYSLOG_ACTION_OPEN       1
#define SYSLOG_ACTION_READ       2
#define SYSLOG_ACTION_READ_ALL   3
#define SYSLOG_ACTION_READ_CLEAR 4
#define SYSLOG_ACTION_CLEAR      5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON 7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

/* prctl operations */
#define PR_SET_NAME        15
#define PR_GET_NAME        16
#define PR_SET_SECCOMP     22
#define PR_GET_SECCOMP     23
#define PR_SET_PDEATHSIG    1
#define PR_GET_PDEATHSIG    2
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_SET_SECUREBITS  41
#define PR_GET_SECUREBITS  42
#define PR_SET_DUMPABLE     4  /* set dumpable flag (0/1/2) */
#define PR_GET_DUMPABLE     3  /* get dumpable flag */
#define PR_SET_PTRACER      0x59616d61  /* YAMA: allow PID to trace (0 = none, -1 = any) */
#define PR_GET_PTRACER      0x59616d62  /* YAMA: get allowed tracer PID */

/* execveat flags */
#define AT_EMPTY_PATH  0x1000
#define AT_SYMLINK_NOFOLLOW 0x100

/* ioctl commands (generic) */
#define TIOCGWINSZ     0x5413
#define TIOCSWINSZ     0x5414

/* sysconf names (Linux compatible) */
#define _SC_CLK_TCK         2
#define _SC_PAGESIZE        30
#define _SC_NPROCESSORS_CONF  83
#define _SC_NPROCESSORS_ONLN  84

/* Clone / threading */
#define SYS_CLONE           231
#define SYS_UNSHARE         394
#define SYS_SETNS           395
#define SYS_GETTID          232
/* Process Credentials & Scheduling */
#define SYS_CAPGET            570
#define SYS_CAPSET            571
#define SYS_GETRESUID         350
#define SYS_TKILL           233
#define SYS_EXECVE          234
/* Threading (pthread support) */
#define SYS_THREAD_CREATE    550  /* thread_create(fn, arg) → thread_id */
#define SYS_THREAD_JOIN      551  /* thread_join(thread_id, &retval) → 0 or error */
#define SYS_THREAD_EXIT      552  /* thread_exit(retval) → never returns */

/* pselect6 / ppoll — safer select/poll with atomic signal mask */
#define SYS_PSELECT6         375  /* pselect6(nfds, rfds, wfds, efds, timeout, &data) */
#define SYS_PPOLL            376  /* ppoll(fds, nfds, timeout, sigmask) */

/* Restartable sequences (rseq) */
#define SYS_RSEQ             396  /* rseq(rseq_addr, rseq_len, rseq_sig, flags) → 0 or -errno */

/* Extended scheduler attributes (sched_setattr/sched_getattr — Linux sched_getattr/sched_setattr) */
#define SYS_SCHED_SETATTR    397  /* sched_setattr(pid, attr, flags) → 0 or -1 */
#define SYS_SCHED_GETATTR    398  /* sched_getattr(pid, attr, size, flags) → 0 or -1 */
#define SYS_KCOV             399  /* kcov(cmd, arg2) → 0 or -1 (Item 208) */

/* Swap — block device swap (Item 223) */
#define SYS_SWAPON           500  /* swapon(path) → 0 or -errno */
#define SYS_SWAPOFF          501  /* swapoff(path) → 0 or -errno */

/* Memory sealing — mseal() is a Linux 6.10+ syscall that makes VA ranges
 * immutable against further mprotect, munmap, or mmap changes */
#define SYS_MSEAL            502  /* mseal(addr, len, flags) → 0 or -errno */

/* seccomp(2) — standalone syscall for BPF-based syscall filtering.
 *    operation: SECCOMP_SET_MODE_STRICT=1, SECCOMP_SET_MODE_FILTER=2
 *    flags:     SECCOMP_FILTER_FLAG_TSYNC=1 */
#define SYS_SECCOMP          503  /* seccomp(operation, flags, args) → 0 or -errno */

/* pidfd operations (Linux-compatible) */
#define SYS_PIDFD_OPEN           434  /* pidfd_open(pid, flags) → fd or -errno */
#define SYS_PIDFD_SEND_SIGNAL    424  /* pidfd_send_signal(pidfd, sig, info, flags) → 0 or -errno */
#define SYS_PIDFD_GETFD          438  /* pidfd_getfd(pidfd, target_fd, flags) → new fd or -errno */

/* close_range(2) — batch close file descriptors */
#define SYS_CLOSE_RANGE          436  /* close_range(first, last, flags) → 0 or -errno */

/* CLOSE_RANGE flags */
#define CLOSE_RANGE_CLOEXEC      2    /* Set close-on-exec on fds in range instead of closing */

/* mount_setattr(2) — set mount attributes */
#define SYS_MOUNT_SETATTR        442  /* mount_setattr(fd, path, flags, attr, size) → 0 or -errno */

/* mount_attr flags (Linux-compatible) */
#define MOUNT_ATTR_RDONLY        0x00000001  /* Mount read-only */
#define MOUNT_ATTR_NOSUID        0x00000002  /* Ignore suid and sgid bits */
#define MOUNT_ATTR_NODEV         0x00000004  /* Disallow access to device files */
#define MOUNT_ATTR_NOEXEC        0x00000008  /* Disallow program execution */
#define MOUNT_ATTR_RELATIME      0x00000010  /* Update atime relative to mtime/ctime */

/* Process spawning — lightweight create+exec (Item 306) */
#define SYS_POSIX_SPAWN      777  /* posix_spawn(path, argv, envp, flags) → pid or -errno */

/* ── kexec (Item 362) ─────────────────────────────────────────── */
#define SYS_KEXEC_LOAD       778  /* kexec_load(phys_addr, entry, flags) → 0 or -1 */

/* ── io_uring syscalls ───────────────────────────────────────────── */
#define SYS_IO_URING_SETUP    425  /* io_uring_setup(entries, params) → fd */
#define SYS_IO_URING_ENTER    426  /* io_uring_enter(fd, to_submit, min_complete, flags) */
#define SYS_IO_URING_REGISTER 427  /* io_uring_register(fd, opcode, arg, nr_args) */

/*
 * syscall_dispatch is a kernel-internal function called ONLY from the
 * userspace API: user code and libc must go through the `syscall` instruction.
 * The declaration is kept here so that syscall.c (which defines it) and
 * kernel.c (which includes syscall.h) can see it, but it should never be
 * called directly from libc or application code.
 */
#ifdef KERNEL_INTERNAL
void syscall_init(void);
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);
/* PRNG for kernel subsystems (ASLR, etc.) */
uint64_t prng_rand64(void);
void prng_add_entropy(uint64_t entropy);
/* Timer fd tick — called from timer interrupt */
void timerfd_tick(void);
/* POSIX per-process timer tick — called from timer interrupt */
void posix_timer_tick(void);
/* Initialize production subsystems (socket, epoll, timers, mq) */
void production_subsystems_init(void);
#endif

#endif
