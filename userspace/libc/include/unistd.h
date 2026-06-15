#ifndef _UNISTD_H
#define _UNISTD_H

/* Syscall number defines (matching kernel's syscall.h) */
#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_EXIT      4
#define SYS_GETPID    5
#define SYS_KILL      6
#define SYS_BRK       7
#define SYS_STAT      8
#define SYS_MKDIR     9
#define SYS_UNLINK    10
#define SYS_YIELD     12
#define SYS_GETDENTS64 78
#define SYS_WAITPID   119
#define SYS_CHDIR     204
#define SYS_GETCWD    205
#define SYS_FORK      211
#define SYS_SIGNAL    213
#define SYS_LSEEK     214
#define SYS_SETPGID   221
#define SYS_GETPGID   222
#define SYS_EXECVE    234
#define SYS_DUP       240
#define SYS_DUP2      241
#define SYS_NANOSLEEP 246
#define SYS_UNAME     248
#define SYS_PIPE      249
#define SYS_GETPPID   250
#define SYS_ALARM     251
#define SYS_GETUID    254
#define SYS_GETEUID   255
#define SYS_GETGID    256
#define SYS_GETEGID   257
#define SYS_RMDIR     258
#define SYS_RENAME    259
#define SYS_CHMOD     260
#define SYS_ACCESS    253
#define SYS_STATFS    346
#define SYS_FTRUNCATE 283
#define SYS_CLOCK_GETTIME 333
#define SYS_SYSINFO   349
#define SYS_UTIMENSAT 344
#define SYS_SYNC      311
#define SYS_REBOOT    267
#define SYS_GETHOSTNAME 269
#define SYS_UMASK     270
#define SYS_IOCTL     278
#define SYS_FSTATAT   290
#define SYS_READLINKAT 294
#define SYS_POSIX_SPAWN 777

/* Network syscalls */
#define SYS_NET_PRESENT   124
#define SYS_NET_GET_MAC   125
#define SYS_NET_GET_IP    126
#define SYS_NET_GET_GW    127
#define SYS_NET_GET_MASK  128
#define SYS_NET_DNS       129
#define SYS_NET_PING      130
#define SYS_NET_UDP_SEND  131
#define SYS_NET_HTTP_GET  132
#define SYS_NET_ARP_LIST  133
#define SYS_NET_TCP_LISTEN      183
#define SYS_NET_TCP_ACCEPT      184
#define SYS_NET_TCP_SEND_CONN   185
#define SYS_NET_TCP_RECV_CONN   186
#define SYS_NET_TCP_CLOSE_CONN  187
#define SYS_NET_TCP_UNLISTEN    188
#define SYS_NET_TCP_CONNECT     189
#define SYS_NET_UDP_LISTEN      198
#define SYS_NET_UDP_RECV        199
#define SYS_NET_UDP_UNLISTEN    200
#define SYS_NET_CONNLIST        212

/* Standard file descriptors */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define NULL ((void *)0)

/* Open flags */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200
#define O_APPEND    0x400

/* Seek flags */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* Waitpid flags */
#define WNOHANG     1
#define WUNTRACED   2

/* Signal numbers */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTKFLT   16
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22

/* AT_* constants for *at syscalls */
#define AT_FDCWD            (-100)
#define AT_EMPTY_PATH       0x1000
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200

/* struct stat — returned by stat() and fstat() */
struct stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long long st_rdev;
    unsigned long long st_size;
    unsigned long long st_blksize;
    unsigned long long st_blocks;
    unsigned long long st_atime;
    unsigned long long st_mtime;
    unsigned long long st_ctime;
};

/* struct dirent — returned by getdents64 */
struct dirent {
    unsigned long long d_ino;
    unsigned long long d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};

/* struct timespec — for nanosleep */
struct timespec {
    unsigned long long tv_sec;
    unsigned long long tv_nsec;
};

/* struct statfs — returned by statfs() */
struct statfs {
    unsigned long long f_type;
    unsigned long long f_bsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
    unsigned long long f_files;
    unsigned long long f_ffree;
    unsigned long long f_fsid;
    unsigned long long f_namelen;
    unsigned long long f_frsize;
    unsigned long long f_flags;
    unsigned long long f_spare[4];
};

/* struct sysinfo — returned by sysinfo() */
struct sysinfo {
    unsigned long long uptime;
    unsigned long long loads[3];
    unsigned long long totalram;
    unsigned long long freeram;
    unsigned long long sharedram;
    unsigned long long bufferram;
    unsigned long long totalswap;
    unsigned long long freeswap;
    unsigned short      procs;
    unsigned short      totalhigh;
    unsigned short      freehigh;
    unsigned int        mem_unit;
};

/* struct utsname — for uname */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* Additional syscall numbers (not yet in unistd.h but needed by commands) */
#define SYS_FS_CHOWN      109  /* fs_chown(path, uid, gid) */
#define SYS_MKNOD         271  /* mknod(path, mode, dev) */
#define SYS_MOUNT         281  /* mount(src, target, fstype, flags, data) */
#define SYS_UMOUNT        282  /* umount(target) */
#define SYS_INIT_MODULE   366  /* init_module(path, params) */
#define SYS_DELETE_MODULE 368  /* delete_module(name, flags) */
#define SYS_QUERY_MODULE  369  /* query_module(name, info_buf, buf_size) */
#define SYS_CHROOT        377  /* chroot(path) */
#define SYS_SWAPON        500  /* swapon(path) */
#define SYS_SWAPOFF       501  /* swapoff(path) */

/* Function prototypes */
extern int write(int fd, const void *buf, unsigned long count);
extern int read(int fd, void *buf, unsigned long count);
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern void exit(int status);
extern long brk(void *addr);
extern int getpid(void);
extern int kill(int pid, int sig);
extern int stat(const char *path, struct stat *buf);
extern int fstat(int fd, struct stat *buf);
extern int mkdir(const char *path, int mode);
extern int unlink(const char *path);
extern int rmdir(const char *path);
extern int readlink(const char *path, char *buf, unsigned long size);
extern int readlinkat(int dirfd, const char *path, char *buf, unsigned long bufsize);
extern int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
extern int getdents64(int fd, void *buf, unsigned long count);
extern int waitpid(int pid, int *status, int options);
extern int execve(const char *path, char *const argv[], char *const envp[]);
extern int fork(void);
extern int chdir(const char *path);
extern int getcwd(char *buf, unsigned long size);
extern int nanosleep(void *req, void *rem);
extern int posix_spawn(const char *path, char *const argv[], char *const envp[]);
extern int dup(int oldfd);
extern int dup2(int oldfd, int newfd);
extern void yield(void);
extern int ioctl(int fd, unsigned long cmd, void *arg);
extern long lseek(int fd, long offset, int whence);
extern int pipe(int fds[2]);
extern int signal(int signum, void (*handler)(int));
extern int uname(struct utsname *buf);
extern unsigned int alarm(unsigned int seconds);
extern int getppid(void);
extern int getpgid(int pid);
extern int setpgid(int pid, int pgid);
extern int getuid(void);
extern int geteuid(void);
extern int getgid(void);
extern int getegid(void);
extern unsigned int umask(unsigned int mask);

/* New syscall wrappers */
extern int rename(const char *old, const char *new);
extern int chmod(const char *path, unsigned int mode);
extern int access(const char *path, int mode);
extern int statfs(const char *path, struct statfs *buf);
extern int ftruncate(int fd, unsigned long length);
extern int clock_gettime(int clockid, struct timespec *tp);
extern int sysinfo(struct sysinfo *info);
extern int utimensat(int dirfd, const char *path, const struct timespec *times, int flags);
extern int sync(void);
extern int reboot(void);
extern int gethostname(char *name, unsigned long len);

/* Additional syscall wrappers */
extern int fs_chown(const char *path, unsigned int uid, unsigned int gid);
extern int mknod(const char *path, unsigned int mode, unsigned long dev);
extern int mount(const char *src, const char *target, const char *fstype, unsigned long flags, const void *data);
extern int umount(const char *target);
extern int init_module(const char *path, const char *params);
extern int delete_module(const char *name, unsigned long flags);
extern int query_module(const char *name, void *info_buf, unsigned long buf_size);
extern int chroot(const char *path);
extern int swapon(const char *path);
extern int swapoff(const char *path);

/* Process groups */
extern int setpgrp(void);
extern int getpgrp(void);

/* Network syscall wrappers */
extern int net_present(void);
extern int net_get_mac(unsigned char *mac);
extern int net_get_ip(unsigned char *ip);
extern unsigned int net_get_gw(void);
extern unsigned int net_get_mask(void);
extern int net_dns(const char *host);
extern int net_ping(unsigned int ip);
extern int net_udp_send(unsigned int dst_ip, unsigned short src_port, unsigned short dst_port, const void *data, unsigned int len);
extern int net_http_get(const char *host, unsigned short port, const char *path, char *buf, unsigned int bufsz);
extern int net_tcp_listen(unsigned short port);
extern int net_tcp_accept(unsigned short port, unsigned int timeout_ticks);
extern int net_tcp_send_conn(int conn_id, const void *buf, unsigned int len);
extern int net_tcp_recv_conn(int conn_id, void *buf, unsigned int bufsz);
extern int net_tcp_close_conn(int conn_id);
extern int net_tcp_unlisten(unsigned short port);
extern int net_tcp_connect(unsigned int ip, unsigned short port);
extern int net_udp_listen(unsigned short port);
extern int net_udp_recv(unsigned short port, void *buf, unsigned int bufsz, unsigned int *src_ip, unsigned short *src_port);
extern int net_udp_unlisten(unsigned short port);
extern int net_connlist(void);
extern int net_arp_list(void);

#endif /* _UNISTD_H */
