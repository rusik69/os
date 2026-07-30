#ifndef ERRNO_H
#define ERRNO_H

/*
 * ── Error number definitions ──
 *
 * These are the standard POSIX errno codes returned by system calls.
 * Negative return values from syscalls carry the negated errno number;
 * the C library wrapper negates them back and stores the positive value
 * in errno(3).  Zero always means success.
 *
 * Layout notes:
 *   1-42   — Traditional UNIX errno values (POSIX.1-1990 + POSIX.1-2001)
 *   52-132 — SVR4 / POSIX.1-2008 additions (key mgmt, networking, etc.)
 *   134    — Kernel-specific: filesystem corruption indicator
 *   512+   — Kernel-internal restart codes (NEVER exposed to userspace)
 *
 * Many gaps exist (15, 26, 33, 41, 44-51, 53-59, 64-65, 68-70, 72-73,
 * 78-83, 128-131 on some platforms) — these are reserved / unassigned.
 */

/* ── Base POSIX.1-1990 error codes ── */

/* 1  Operation not permitted — caller lacks the required privilege. */
#define EPERM 1
/* 2  No such file or directory — path component does not exist. */
#define ENOENT 2
/* 3  No such process — PID / TGID not found. */
#define ESRCH 3
/* 4  Interrupted system call — signal caught during blocking op. */
#define EINTR 4
/* 5  I/O error — low-level hardware or driver communication failure. */
#define EIO 5
/* 6  No such device or address — device or remote address not present. */
#define ENXIO 6
/* 7  Argument list too long — execve(2) argv/envp overflow. */
#define E2BIG 7
/* 8  Exec format error — unrecognised ELF magic / class. */
#define ENOEXEC 8
/* 9  Bad file descriptor — fd is not open or not valid for this task. */
#define EBADF 9
/* 10 No child processes — wait(2) with no (unwaited) children. */
#define ECHILD 10
/* 11 Resource temporarily unavailable — try again later (non-blocking I/O). */
#define EAGAIN 11
/* 12 Cannot allocate memory — physical or virtual memory exhausted. */
#define ENOMEM 12
/* 13 Permission denied — file permissions or DAC/MAC rejection. */
#define EACCES 13
/* 14 Bad address — pointer is outside the calling task's accessible address space. */
#define EFAULT 14
/* 16 Device or resource busy — resource locked or already in use. */
#define EBUSY 16
/* 17 File exists — O_CREAT | O_EXCL and target already exists. */
#define EEXIST 17
/* 18 Cross-device link — rename(2) / link(2) across different mount points. */
#define EXDEV 18
/* 19 No such device — driver not loaded or device not attached. */
#define ENODEV 19
/* 20 Not a directory — path component is not a dir when one was required. */
#define ENOTDIR 20
/* 21 Is a directory — operation (e.g., write) not allowed on a directory. */
#define EISDIR 21
/* 22 Invalid argument — one or more syscall arguments are invalid. */
#define EINVAL 22
/* 23 File table overflow — system-wide open-file limit reached. */
#define ENFILE 23
/* 24 Too many open files — per-task RLIMIT_NOFILE reached. */
#define EMFILE 24
/* 25 Inappropriate ioctl for device — ioctl(2) request not supported. */
#define ENOTTY 25
/* 27 File too large — beyond the maximum size supported by the FS. */
#define EFBIG 27
/* 28 No space left on device — block allocation exhausted. */
#define ENOSPC 28
/* 29 Illegal seek — lseek(2) on a non-seekable fd (pipe, socket). */
#define ESPIPE 29
/* 30 Read-only filesystem — write attempted on a read-only mount. */
#define EROFS 30
/* 31 Too many links — hard-link count would exceed the per-inode limit. */
#define EMLINK 31
/* 32 Broken pipe — write to a pipe / socket with no reader. Generates SIGPIPE. */
#define EPIPE 32
/* 34 Numerical result out of range — value too large or too small. */
#define ERANGE 34
/* 35 Resource deadlock avoided — lock ordering would cause a deadlock. */
#define EDEADLK 35
/* 36 Filename too long — exceeds NAME_MAX (255) or PATH_MAX (4096). */
#define ENAMETOOLONG 36
/* 37 No locks available — POSIX lock table full. */
#define ENOLCK 37
/* 38 Function not implemented — syscall number or operation not available. */
#define ENOSYS 38
/* 39 Directory not empty — rmdir(2) on a non-empty directory. */
#define ENOTEMPTY 39
/* 40 Too many levels of symbolic links — symlink chain exceeded limit. */
#define ELOOP 40
/* 42 No message of desired type — msgrcv(2) with a matching mtype not found. */
#define ENOMSG 42
/* 43 Identifier removed — IPC identifier was deleted while waiting. */
#define EIDRM 43

/* ── POSIX.1-2001 additions (SVR4-derived) ── */

/* 52 Bad exchange — XENIX / SVR4 file-exchange error. */
#define EBADE 52
/* 52-59; 64-73; 78, 82-83 reserved */

/* 60 Device not a stream — STREAMS operation on a non-STREAMS device. */
#define ENOSTR 60
/* 61 No data available — STREAMS / out-of-band data not ready. */
#define ENODATA 61
/* 62 Timer expired — timer_settime(2) or STREAMS timeout. */
#define ETIME 62
/* 63 Out of streams resources — STREAMS queue limit reached. */
#define ENOSR 63
/* 66 Remote address changed — NFS / RPC remote server changed address. */
#define EREMOTE 66
/* 67 Link has been severed — NFS / RPC connection lost. */
#define ENOLINK 67
/* 71 Protocol error — device or RPC protocol violation. */
#define EPROTO 71
/* 74 Bad message — corrupted or malformed data in IPC / STREAMS. */
#define EBADMSG 74
/* 75 Value too large for defined data type — arithmetic or data-type overflow. */
#define EOVERFLOW 75
/* 76 Name not unique — link target name collision. */
#define ENOTUNIQ 76
/* 77 File descriptor in bad state — corrupted or closed unexpectedly. */
#define EBADFD 77
/* 77-83 reserved */

/* 84 Illegal byte sequence — multibyte character encoding error. */
#define EILSEQ 84
/* 85 Interrupted system call should be restarted — internal restart. */
#define ERESTART 85
/* 86 Streams pipe error — STREAMS pipe operation failed. */
#define ESTRPIPE 86

/* ── Networking error codes (POSIX.1-2001) ── */

/* 87 Too many users — user quota for the socket layer is exhausted. */
#define EUSERS 87
/* 88 Socket operation on non-socket — fd is not a socket. */
#define ENOTSOCK 88
/* 89 Destination address required — connect(2) / sendto(2) without address. */
#define EDESTADDRREQ 89
/* 90 Message too long — datagram exceeds socket buffer or MTU. */
#define EMSGSIZE 90
/* 91 Protocol wrong type for socket — e.g., SOCK_STREAM with UDP. */
#define EPROTOTYPE 91
/* 92 Protocol not available — protocol family not configured. */
#define ENOPROTOOPT 92
/* 93 Protocol not supported — protocol family/number not registered. */
#define EPROTONOSUPPORT 93
/* 94 Socket type not supported — e.g., SOCK_RAW not enabled. */
#define ESOCKTNOSUPPORT 94
/* 95 Operation not supported — socket option / ioctl not implemented. */
#define EOPNOTSUPP 95
/* 96 Protocol family not supported — AF_* value not registered. */
#define EPFNOSUPPORT 96
/* 97 Address family not supported by protocol — AF_* / protocol mismatch. */
#define EAFNOSUPPORT 97
/* 98 Address already in use — bind(2) to an already-bound address. */
#define EADDRINUSE 98
/* 99 Cannot assign requested address — local address not available. */
#define EADDRNOTAVAIL 99

/* ── Network connectivity errors ── */

/* 100 Network is down — link-level carrier loss. */
#define ENETDOWN 100
/* 101 Network is unreachable — no route to the destination. */
#define ENETUNREACH 101
/* 102 Network dropped connection because of reset — remote host reset. */
#define ENETRESET 102
/* 103 Software caused connection abort — internal abort (e.g., timeout). */
#define ECONNABORTED 103
/* 104 Connection reset by peer — remote endpoint closed / rebooted. */
#define ECONNRESET 104
/* 105 No buffer space available — socket buffer or mbuf allocation failed. */
#define ENOBUFS 105
/* 106 Transport endpoint is already connected — connect(2) on connected socket. */
#define EISCONN 106
/* 107 Transport endpoint is not connected — send(2) / recv(2) without connect. */
#define ENOTCONN 107
/* 108 Cannot send after transport endpoint shutdown — write after shutdown(SHUT_WR). */
#define ESHUTDOWN 108
/* 109 Too many references — splice / SCM_RIGHTS fd count exceeded. */
#define ETOOMANYREFS 109
/* 110 Connection timed out — retransmit limit reached, no response. */
#define ETIMEDOUT 110
/* 111 Connection refused — no listener on the remote port. */
#define ECONNREFUSED 111
/* 112 Host is down — remote host not reachable at link level. */
#define EHOSTDOWN 112
/* 113 No route to host — routing table has no entry for the destination. */
#define EHOSTUNREACH 113
/* 114 Operation already in progress — non-blocking connect(2) already started. */
#define EALREADY 114
/* 115 Operation now in progress — non-blocking connect(2) started. */
#define EINPROGRESS 115

/* ── Filesystem / NFS status codes ── */

/* 116 Stale NFS file handle — file was deleted on the server. */
#define ESTALE 116
/* 117 Structure needs cleaning — extX journal recovery required. */
#define EUCLEAN 117
/* 118 Not a XENIX named type file — SVR4 XENIX compatibility. */
#define ENOTNAM 118
/* 119 No XENIX semaphores available — SVR4 XENIX compatibility. */
#define ENAVAIL 119
/* 120 Is a named type file — SVR4 XENIX compatibility. */
#define EISNAM 120
/* 121 Remote I/O error — NFS / network storage I/O failure. */
#define EREMOTEIO 121
/* 122 Disk quota exceeded — user / group block or inode quota hit. */
#define EDQUOT 122
/* 123 No medium found — removable media tray is empty. */
#define ENOMEDIUM 123
/* 124 Wrong medium type — incorrect disc / cartridge inserted. */
#define EMEDIUMTYPE 124

/* ── Cancellation / Key management ── */

/* 125 Operation canceled — aio_cancel(2) or thread cancellation. */
#define ECANCELED 125
/* 126 Required key not available — keyring / keyctl lookup failed. */
#define ENOKEY 126
/* 127 Key has expired — cryptographic key lifetime expired. */
#define EKEYEXPIRED 127
/* 128 Key has been revoked — key explicitly revoked or destroyed. */
#define EKEYREVOKED 128
/* 129 Key was rejected by service — keyring permission or trust check failed. */
#define EKEYREJECTED 129

/* ── Robust-futex / owner-death tracking (POSIX.1-2008) ── */

/* 130 Owner died — robust mutex owner exited while holding the lock. */
#define EOWNERDEAD 130
/* 131 State not recoverable — robust mutex chain is corrupted. */
#define ENOTRECOVERABLE 131

/* ── Linux-specific ── */

/* 132 Operation not possible due to RF-kill — wireless radio hard-blocked. */
#define ERFKILL 132
/* 133 Memory page has hardware error — machine check / poisoned page accessed. */
#define EHWPOISON 133

/* Filesystem corruption — used by FS drivers to signal unrecoverable
 * structural damage before forcing a read-only remount. */
#define EFSCORRUPTED 134

/* ── Kernel-internal restart codes (NEVER returned to userspace) ──
 *
 * These are positive internal error codes used by kernel syscall handlers
 * to indicate that a blocking operation was interrupted by a signal.
 * They MUST be converted to -EINTR (or cause a transparent restart if
 * SA_RESTART is set) before the return value reaches userspace.
 *
 * Values are deliberately high (> 256) to avoid colliding with errno
 * values that might be exposed to userspace.
 */
#define ERESTARTSYS 512    /* restart if SA_RESTART, else convert to -EINTR */
#define ERESTARTNOINTR 513 /* restart unconditionally (no -EINTR) */
#define ERESTARTNOHAND 514 /* restart only if no user handler installed */

#endif /* ERRNIO_H */
