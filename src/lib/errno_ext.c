#include "errno_ext.h"
#include "printf.h"
#include "kernel.h"

/* Global errno storage */
int __errno_value = 0;

int *__errno_location(void) {
    return &__errno_value;
}

static const char * const err_desc[] = {
    [0]            = "Success",
    [EPERM]        = "Operation not permitted",
    [ENOENT]       = "No such file or directory",
    [ESRCH]        = "No such process",
    [EINTR]        = "Interrupted system call",
    [EIO]          = "I/O error",
    [ENXIO]        = "No such device or address",
    [E2BIG]        = "Argument list too long",
    [ENOEXEC]      = "Exec format error",
    [EBADF]        = "Bad file number",
    [ECHILD]       = "No child processes",
    [EAGAIN]       = "Try again",
    [ENOMEM]       = "Out of memory",
    [EACCES]       = "Permission denied",
    [EFAULT]       = "Bad address",
    [EBUSY]        = "Device or resource busy",
    [EEXIST]       = "File exists",
    [EXDEV]        = "Cross-device link",
    [ENODEV]       = "No such device",
    [ENOTDIR]      = "Not a directory",
    [EISDIR]       = "Is a directory",
    [EINVAL]       = "Invalid argument",
    [ENFILE]       = "File table overflow",
    [EMFILE]       = "Too many open files",
    [ENOTTY]       = "Not a typewriter",
    [EFBIG]        = "File too large",
    [ENOSPC]       = "No space left on device",
    [ESPIPE]       = "Illegal seek",
    [EROFS]        = "Read-only file system",
    [EMLINK]       = "Too many links",
    [EPIPE]        = "Broken pipe",
    [ERANGE]       = "Math result not representable",
    [EDEADLK]      = "Resource deadlock would occur",
    [ENAMETOOLONG] = "File name too long",
    [ENOLCK]       = "No record locks available",
    [ENOSYS]       = "Function not implemented",
    [ENOTEMPTY]    = "Directory not empty",
    [ELOOP]        = "Too many symbolic links encountered",
    [ENOMSG]       = "No message of desired type",
    [EIDRM]        = "Identifier removed",
    [ENOSTR]       = "Device not a stream",
    [ENODATA]      = "No data available",
    [ETIME]        = "Timer expired",
    [ENOSR]        = "Out of streams resources",
    [EREMOTE]      = "Object is remote",
    [ENOLINK]      = "Link has been severed",
    [EPROTO]       = "Protocol error",
    [EBADE]        = "Bad exchange descriptor",
    [EBADFD]       = "File descriptor in bad state",
    [ENOTUNIQ]     = "Name not unique on network",
    [EBADMSG]      = "Bad message",
    [EOVERFLOW]    = "Value too large for defined data type",
    [EILSEQ]       = "Illegal byte sequence",
    [ERESTART]     = "Interrupted system call should be restarted",
    [ESTRPIPE]     = "Streams pipe error",
    [EUSERS]       = "Too many users",
    [ENOTSOCK]     = "Socket operation on non-socket",
    [EDESTADDRREQ] = "Destination address required",
    [EMSGSIZE]     = "Message too long",
    [EPROTOTYPE]   = "Protocol wrong type for socket",
    [ENOPROTOOPT]  = "Protocol not available",
    [EPROTONOSUPPORT] = "Protocol not supported",
    [ESOCKTNOSUPPORT] = "Socket type not supported",
    [EOPNOTSUPP]   = "Operation not supported on transport endpoint",
    [EPFNOSUPPORT] = "Protocol family not supported",
    [EAFNOSUPPORT] = "Address family not supported by protocol",
    [EADDRINUSE]   = "Address already in use",
    [EADDRNOTAVAIL] = "Cannot assign requested address",
    [ENETDOWN]     = "Network is down",
    [ENETUNREACH]  = "Network is unreachable",
    [ENETRESET]    = "Network dropped connection because of reset",
    [ECONNABORTED] = "Software caused connection abort",
    [ECONNRESET]   = "Connection reset by peer",
    [ENOBUFS]      = "No buffer space available",
    [EISCONN]      = "Transport endpoint is already connected",
    [ENOTCONN]     = "Transport endpoint is not connected",
    [ESHUTDOWN]    = "Cannot send after transport endpoint shutdown",
    [ETOOMANYREFS] = "Too many references: cannot splice",
    [ETIMEDOUT]    = "Connection timed out",
    [ECONNREFUSED] = "Connection refused",
    [EHOSTDOWN]    = "Host is down",
    [EHOSTUNREACH] = "No route to host",
    [EALREADY]     = "Operation already in progress",
    [EINPROGRESS]  = "Operation now in progress",
    [ESTALE]       = "Stale file handle",
    [EUCLEAN]      = "Structure needs cleaning",
    [ENOTNAM]      = "Not a XENIX named type file",
    [ENAVAIL]      = "No XENIX semaphores available",
    [EISNAM]       = "Is a named type file",
    [EREMOTEIO]    = "Remote I/O error",
    [EDQUOT]       = "Quota exceeded",
    [ENOMEDIUM]    = "No medium found",
    [EMEDIUMTYPE]  = "Wrong medium type",
    [ECANCELED]    = "Operation canceled",
    [ENOKEY]       = "Required key not available",
    [EKEYEXPIRED]  = "Key has expired",
    [EKEYREVOKED]  = "Key has been revoked",
    [EKEYREJECTED] = "Key was rejected by service",
    [EOWNERDEAD]   = "Owner died",
    [ENOTRECOVERABLE] = "State not recoverable",
    [ERFKILL]      = "Operation not possible due to RF-kill",
    [EHWPOISON]    = "Memory page has hardware error",
    [EFSCORRUPTED]  = "Filesystem corruption",
};

#define ERR_DESC_COUNT ARRAY_SIZE(err_desc)

/*
 * strerror — return string description for error number.
 * Returns "Unknown error N" for unrecognized numbers.
 */
char *strerror(int errnum) {
    if (errnum >= 0 && (size_t)errnum < ERR_DESC_COUNT && err_desc[errnum])
        return (char *)(uintptr_t)err_desc[errnum];

    /* Return a static buffer for unknown errors */
    static char unknown[32];
    int n = snprintf(unknown, sizeof(unknown), "Unknown error %d", errnum);
    if (n < 0) unknown[0] = '\0';
    return unknown;
}

/*
 * perror — print a message followed by ": " and the error string for errno.
 */
void perror(const char *s) {
    if (s && *s)
        kprintf("%s: %s\n", s, strerror(__errno_value));
    else
        kprintf("%s\n", strerror(__errno_value));
}

/* ── errno_str ─────────────────────────────── */
const char* errno_str(int err)
{
    return strerror(err);
}
/* ── errno_set ─────────────────────────────── */
int errno_set(int err)
{
    __errno_value = err;
    return 0;
}
