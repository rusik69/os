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
#define SYS_UMASK     270
#define SYS_IOCTL     278
#define SYS_FSTATAT   290
#define SYS_READLINKAT 294
#define SYS_POSIX_SPAWN 777

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

/* struct utsname — for uname */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

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

/* Process groups */
extern int setpgrp(void);
extern int getpgrp(void);

#endif /* _UNISTD_H */
