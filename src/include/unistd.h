#ifndef UNISTD_H
#define UNISTD_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard POSIX types */
typedef int64_t off_t;
typedef int64_t ssize_t;

/* ── Standard file descriptors ──────────────────────────────────── */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* ── lseek whence values ────────────────────────────────────────── */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ── access() mode flags ────────────────────────────────────────── */
#define F_OK 0  /* test for existence */
#define R_OK 4  /* test for read permission */
#define W_OK 2  /* test for write permission */
#define X_OK 1  /* test for execute permission */

/* ── sysconf() constants ────────────────────────────────────────── */
#ifndef _SC_PAGESIZE
#define _SC_CLK_TCK            2
#define _SC_PAGESIZE           30
#define _SC_NPROCESSORS_CONF   83
#define _SC_NPROCESSORS_ONLN   84
#endif
/* Custom extensions */
#define _SC_PHYS_PAGES         4
#define _SC_AVPHYS_PAGES       5
#define _SC_OPEN_MAX           7
#define _SC_STREAM_MAX         8
#define _SC_TZNAME_MAX         9

/* ── Process lifetime ───────────────────────────────────────────── */

/** Fork the current process.
 *  Returns 0 in the child, child PID in the parent, or -1 on error. */
int fork(void);

/** Execute a program.  path is the full path to the ELF binary.
 *  argv is an array of argument strings (NULL-terminated).
 *  envp is an array of environment strings (NULL-terminated).
 *  On success, does not return.  Returns -1 on error. */
int execve(const char *path, char *const argv[], char *const envp[]);

/** Execute a program, searching PATH. */
int execvp(const char *file, char *const argv[]);

/** Execute a program with variadic arguments (NULL-terminated).
 *  Only the first argument is the path; subsequent args are passed
 *  to the program as argv[1..n]. */
int execl(const char *path, const char *arg, ...);

/** Execute a program with file descriptor, path, argv, envp, flags.
 *  The 'flags' argument can include AT_EMPTY_PATH.
 *  On success, does not return.  Returns -1 on error. */
int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags);

/** Terminate the calling process with the given exit status.
 *  Does not return. */
void _exit(int status) __attribute__((noreturn));

/** Get the process ID of the calling process. */
unsigned int getpid(void);

/** Get the parent process ID. */
unsigned int getppid(void);

/* ── Process groups / sessions ──────────────────────────────────── */

/** Set process group ID. */
int setpgid(unsigned int pid, unsigned int pgid);

/** Get process group ID. */
unsigned int getpgid(unsigned int pid);

/** Create a new session.  Returns session ID on success. */
unsigned int setsid(void);

/** Get the session ID of the given process. */
unsigned int getsid(unsigned int pid);

/* ── Signals ────────────────────────────────────────────────────── */

/** Schedule an alarm signal (SIGALRM) after 'seconds' seconds.
 *  Returns the number of seconds remaining from the previous alarm,
 *  or 0 if no previous alarm was set. */
unsigned int alarm(unsigned int seconds);

/** Suspend the calling process until a signal is delivered.
 *  Always returns -1 with errno set to EINTR. */
int pause(void);

/* ── File operations ────────────────────────────────────────────── */

/** Check file accessibility.  mode is F_OK, R_OK, W_OK, X_OK, or a
 *  bitwise OR of the latter three.  Returns 0 on success, -1 on error. */
int access(const char *path, int mode);

/** Get the current working directory.  Returns buf on success, NULL on
 *  error (if buf is NULL, allocates a buffer of size 'size'). */
char *getcwd(char *buf, size_t size);

/** Change the current working directory to the given path.
 *  Returns 0 on success, -1 on error. */
int chdir(const char *path);

/** Duplicate a file descriptor.  Returns new fd or -1 on error. */
int dup(int oldfd);

/** Duplicate a file descriptor to a specific number.
 *  Returns newfd on success, -1 on error. */
int dup2(int oldfd, int newfd);

/** Close a file descriptor.  Returns 0 on success, -1 on error. */
int close(int fd);

/** Read from a file descriptor. */
ssize_t read(int fd, void *buf, size_t count);

/** Write to a file descriptor. */
ssize_t write(int fd, const void *buf, size_t count);

/** Reposition read/write file offset. */
off_t lseek(int fd, off_t offset, int whence);

/* ── Pipes ──────────────────────────────────────────────────────── */

/** Create a pipe.  pipefd[0] is read end, pipefd[1] is write end.
 *  Returns 0 on success, -1 on error. */
int pipe(int pipefd[2]);

/* ── System configuration ───────────────────────────────────────── */

/** Get system configuration values. */
long sysconf(int name);

/** Get the hostname of the system.  Returns 0 on success, -1 on error. */
int gethostname(char *name, size_t len);

/** Set the hostname of the system.  Requires appropriate privileges.
 *  Returns 0 on success, -1 on error. */
int sethostname(const char *name, size_t len);

/** Get the number of clock ticks per second. */
long _SC_CLK_TCK_value(void);

/* ── Sleeping ───────────────────────────────────────────────────── */

/** Sleep for 'seconds' seconds.  Returns the number of seconds
 *  remaining if interrupted by a signal, or 0 on completion. */
unsigned int sleep(unsigned int seconds);

/** Sleep for usec microseconds.  Returns 0 on success, -1 on error. */
int usleep(unsigned long usec);

/* ── I/O control ────────────────────────────────────────────────── */

/** Control a device.  Returns 0 on success, -1 on error. */
int ioctl(int fd, unsigned long request, ...);

/* ── Truncation ─────────────────────────────────────────────────── */

/** Truncate a file to a specified length. */
int truncate(const char *path, off_t length);

/** Truncate an open file to a specified length. */
int ftruncate(int fd, off_t length);

/* ── Process sync ───────────────────────────────────────────────── */

/** Wait for all children to terminate / Send a sync command.
 *  On this system, sync() flushes filesystem buffers. */
void sync(void);

#ifdef __cplusplus
}
#endif

#endif /* UNISTD_H */
