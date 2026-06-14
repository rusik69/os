#ifndef _UNISTD_H
#define _UNISTD_H

/* Syscall number defines (matching kernel's syscall.h) */
#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_EXIT      4
#define SYS_GETPID    5
#define SYS_BRK       7
#define SYS_WAITPID   119
#define SYS_CHDIR     204
#define SYS_GETCWD    205
#define SYS_NANOSLEEP 246
#define SYS_FORK      211
#define SYS_ELF_EXEC  155

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

/* Function prototypes */
extern int write(int fd, const void *buf, unsigned long count);
extern int read(int fd, void *buf, unsigned long count);
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern void exit(int status);
extern long brk(void *addr);
extern int getpid(void);
extern int waitpid(int pid, int *status, int options);
extern int execve(const char *path, char *const argv[], char *const envp[]);
extern int fork(void);
extern int chdir(const char *path);
extern int getcwd(char *buf, unsigned long size);
extern int nanosleep(void *req, void *rem);
extern int posix_spawn(const char *path, char *const argv[], char *const envp[]);
extern int dup(int oldfd);
extern int dup2(int oldfd, int newfd);

#endif /* _UNISTD_H */
