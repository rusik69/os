#ifndef _FCNTL_H
#define _FCNTL_H

/*
 * fcntl.h — File creation flags and fcntl operations
 *
 * This header documents the file creation (open) flags and fcntl
 * commands used by the OS kernel.  The flag constants themselves are
 * defined in types.h (included below) alongside the base type system.
 *
 * ============================================================================
 * File creation flags (open(2) / openat(2) / creat(2))
 * ============================================================================
 *
 * Every open() call specifies exactly one access mode from the first
 * three constants (mask 0x3).  These are mutually exclusive:
 *
 *   O_RDONLY   0     Open for reading only.  The file position starts
 *                    at offset 0.  Write operations on the returned fd
 *                    return -EBADF.
 *
 *   O_WRONLY   1     Open for writing only.  Read operations return
 *                    -EBADF.  The file is truncated to zero length if
 *                    O_TRUNC is also specified.
 *
 *   O_RDWR     2     Open for both reading and writing.  This is the
 *                    union of O_RDONLY and O_WRONLY.
 *
 * The access mode is extracted via (flags & 3).  Any value > 2 in
 * the low 2 bits is invalid; sys_open rejects such calls with -EINVAL.
 *
 * --- File creation flags (modify the open/create behaviour) -----------
 *
 *   O_CREAT   0100   Create the file if it does not already exist.
 *                    Requires a mode argument (the third parameter of
 *                    open()).  Without O_CREAT, the file must already
 *                    exist.
 *
 *   O_EXCL    0200   When combined with O_CREAT, fail with -EEXIST if
 *                    the file already exists.  Provides atomic creation
 *                    (the check-and-create is a single syscall).
 *                    Without O_CREAT, O_EXCL is undefined.
 *
 *   O_TRUNC   01000  If the file already exists and is a regular file,
 *                    truncate it to zero length on open.  Requires
 *                    write permission on the file.  Ignored for FIFOs
 *                    or terminal devices.
 *
 *   O_APPEND  02000  All write() calls append to the end of the file.
 *                    The file offset is set to the current file size
 *                    before each write, making the operation atomic
 *                    (no lseek race).  Required for log files and
 *                    shared append-only outputs.
 *
 * --- I/O mode flags ---------------------------------------------------
 *
 *   O_NONBLOCK 04000 Open in non-blocking mode.  read() returns -EAGAIN
 *                    instead of blocking when no data is available.
 *                    write() returns -EAGAIN when the write buffer is
 *                    full.  For FIFOs/sockets, open() itself returns
 *                    -ENXIO if no reader/writer is present.  For device
 *                    files, the driver-specific behaviour applies.
 *
 *   O_DSYNC   010000 Write operations complete according to data-integrity
 *                    completion semantics: data is delivered to the
 *                    storage device before write() returns, but file
 *                    metadata (e.g. access time) may be deferred.
 *
 *   O_SYNC   04010000 Synchronous writes — both data and metadata are
 *                    flushed to the storage device before write()
 *                    returns.  This is the strongest write guarantee and
 *                    implies O_DSYNC.
 *
 *   O_TMPFILE 0x80000 Create an unnamed temporary file.  The pathname
 *                    argument must specify a directory; the resulting
 *                    file has no visible directory entry.  Once all
 *                    file descriptors are closed, the file is deleted.
 *                    Our implementation uses bit 19 (0x80000).  Linux
 *                    uses 0x200000 (bit 21), but our O_CLOEXEC occupies
 *                    bit 25, so we cannot use the Linux value.
 *
 * --- Special flags ----------------------------------------------------
 *
 *   O_CLOEXEC 02000000 Set the close-on-exec flag on the new fd.
 *                    When execve() is called, the fd is automatically
 *                    closed.  Without this flag, the fd survives exec
 *                    (unless already FD_CLOEXEC via fcntl F_SETFD).
 *                    Using O_CLOEXEC avoids the TOCTOU race between
 *                    open() and fcntl(F_SETFD) in multi-threaded code.
 *
 * ============================================================================
 * fcntl commands
 * ============================================================================
 *
 * The fcntl() syscall operates on an open file descriptor.  Common
 * commands implemented by the OS kernel (see syscall.c sys_fcntl):
 *
 *   F_DUPFD         Duplicate the fd — return the lowest-numbered
 *                   available fd >= arg.
 *   F_DUPFD_CLOEXEC Like F_DUPFD but set FD_CLOEXEC on the new fd.
 *   F_GETFD         Return the file descriptor flags (FD_CLOEXEC).
 *   F_SETFD         Set the file descriptor flags (arg = FD_CLOEXEC).
 *   F_GETFL         Return the open file status flags (O_* from open).
 *   F_SETFL         Set the open file status flags (O_NONBLOCK, O_APPEND
 *                   are settable; access mode and creation flags are not).
 *   F_GETLK         Test for a POSIX advisory lock conflict without
 *                   blocking.  Fills in struct flock with F_UNLCK if
 *                   no conflict exists, or the conflicting lock info.
 *   F_SETLK         Acquire/release a POSIX advisory lock (non-blocking).
 *                   Returns -EACCES or -EAGAIN if lock is held by another
 *                   process.
 *   F_SETLKW        Like F_SETLK but blocks until the lock is available.
 *   F_GETOWN        Return the process/group ID receiving SIGIO signals.
 *   F_SETOWN        Set the process/group ID for SIGIO delivery.
 *   F_SETSIG        Set the signal number delivered for I/O readiness.
 *                   arg 0 restores SIGIO.
 *
 * ============================================================================
 * File descriptor flags
 * ============================================================================
 *
 *   FD_CLOEXEC  1   Close the fd on execve().  Set via fcntl(F_SETFD),
 *                   or via O_CLOEXEC at open time.
 *
 * ============================================================================
 * struct flock — POSIX advisory file lock
 * ============================================================================
 *
 * Used with F_GETLK, F_SETLK, F_SETLKW:
 *
 *   short l_type     Lock type: F_RDLCK (shared read), F_WRLCK
 *                    (exclusive write), F_UNLCK (unlock).
 *   short l_whence   Base for l_start: SEEK_SET, SEEK_CUR, SEEK_END.
 *   off_t  l_start   Offset relative to l_whence.
 *   off_t  l_len     Length in bytes (0 means lock to EOF).
 *   pid_t  l_pid     PID of process holding the lock (returned by
 *                    F_GETLK).
 *
 * ============================================================================
 * AT_* constants for *at() syscalls
 * ============================================================================
 *
 *   AT_FDCWD     -100  Special dirfd value meaning "current working
 *                      directory" for openat(), fstatat(), etc.
 *   AT_SYMLINK_NOFOLLOW  0x100  Do not follow symlinks.
 *   AT_REMOVEDIR         0x200  Remove directory (unlinkat).
 *   AT_EMPTY_PATH         0x1000  Allow empty pathname for f*at calls.
 *
 * ============================================================================
 * Usage summary
 * ============================================================================
 *
 * Typical open() calls:
 *
 *   fd = open("file", O_RDONLY);                     // read existing file
 *   fd = open("file", O_WRONLY | O_CREAT | O_TRUNC, 0644);  // create/overwrite
 *   fd = open("file", O_RDWR | O_CREAT | O_EXCL, 0644);     // create exclusively
 *   fd = open("file", O_WRONLY | O_APPEND);                  // append-only
 *   fd = open("file", O_RDONLY | O_NONBLOCK);                // non-blocking read
 *   fd = open("/tmp", O_TMPFILE | O_RDWR, 0644);             // anonymous temp file
 */

#include <types.h>

/* ── fcntl commands ─────────────────────────────────────────────────── */
#ifndef F_DUPFD
#define F_DUPFD          0       /* Duplicate fd to lowest available >= arg */
#define F_GETFD          1       /* Get file descriptor flags */
#define F_SETFD          2       /* Set file descriptor flags */
#define F_GETFL          3       /* Get file status flags */
#define F_SETFL          4       /* Set file status flags */
#define F_GETLK          5       /* Get record locking information */
#define F_SETLK          6       /* Set record lock (non-blocking) */
#define F_SETLKW         7       /* Set record lock (blocking) */
#define F_SETOWN         8       /* Set owner of SIGIO */
#define F_GETOWN         9       /* Get owner of SIGIO */
#define F_SETSIG         10      /* Set signal for I/O readiness */
#define F_GETSIG         11      /* Get signal for I/O readiness */
#define F_DUPFD_CLOEXEC  12      /* Like F_DUPFD but set FD_CLOEXEC */
#endif

/* ── File descriptor flags ──────────────────────────────────────────── */
#ifndef FD_CLOEXEC
#define FD_CLOEXEC      1       /* Close-on-exec flag */
#endif

/* ── Lock types for struct flock ────────────────────────────────────── */
#ifndef F_RDLCK
#define F_RDLCK         0       /* Shared (read) lock */
#define F_WRLCK         1       /* Exclusive (write) lock */
#define F_UNLCK         2       /* Release lock */
#endif

/* ── struct flock — POSIX advisory file lock ─────────────────────────── */
#ifndef HAVE_STRUCT_FLOCK
#define HAVE_STRUCT_FLOCK
struct flock {
    short  l_type;      /* F_RDLCK, F_WRLCK, or F_UNLCK */
    short  l_whence;    /* SEEK_SET, SEEK_CUR, SEEK_END */
    off_t  l_start;     /* Offset relative to l_whence */
    off_t  l_len;       /* Length in bytes (0 = to EOF) */
    pid_t  l_pid;       /* PID of process holding the lock (F_GETLK) */
};
#endif

/* ── AT_* constants for *at() syscalls ──────────────────────────────── */
#ifndef AT_FDCWD
#define AT_FDCWD           (-100)  /* Use current working directory */
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100  /* Do not follow symbolic links */
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR        0x200  /* Remove directory (unlinkat) */
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH       0x1000 /* Allow empty path for fstatat */
#endif

#endif /* _FCNTL_H */
