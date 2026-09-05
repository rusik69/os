#ifndef TYPES_H
#define TYPES_H

/*
 * types.h — Kernel type definitions and compiler attributes
 *
 * This header provides the fundamental type system for the OS kernel.
 * It defines standard fixed-width integer types, common POSIX/Unix
 * structures, compiler attribute macros, and helper constants used
 * across all kernel and userspace code.
 *
 * ============================================================================
 * Type categories
 * ============================================================================
 *
 * 1. FIXED-WIDTH INTEGERS (lines below)
 *    Standard C99 types: uint8_t, int8_t, uint16_t, etc.
 *    Use these for all hardware register maps, binary protocols, and
 *    any data where width matters across architectures.
 *
 * 2. SIZE / POINTER TYPES
 *    size_t  — unsigned result of sizeof (unsigned 64-bit on x86-64)
 *    ssize_t — signed variant used for return values (byte count or -errno)
 *    uintptr_t — holds any pointer as an integer (64-bit on x86-64)
 *
 * 3. BOOLEAN (lines 77+)
 *    C99 _Bool + bool typedef, true/false constants.
 *
 * 4. COMPILER ATTRIBUTE MACROS (lines 21-69)
 *    __init / __exit — section placement for init/cleanup code
 *    __printf / __scanf — format-string checking
 *    __must_check — warn on unused return value
 *    __user / __kernel / __iomem — address-space annotations for sparse
 *    likely / unlikely — branch prediction hints
 *
 * 5. COMMON CONSTANTS & HELPERS (lines 78-88)
 *    INT64_MAX, INT32_MAX, UINT64_MAX
 *    KERNEL_VMA_OFFSET, PHYS_TO_VIRT, VIRT_TO_PHYS
 *    PAGE_SIZE
 *
 * 6. FD_SET (lines 91-109)
 *    select() bitmask type and manipulators (FD_ZERO/FD_SET/FD_CLR/FD_ISSET)
 *
 * 7. POSIX STRUCTURES (lines 111-145)
 *    struct timespec  — nanosecond-resolution time
 *    struct timeval   — microsecond-resolution time
 *    struct utsname   — system identity (uname)
 *    struct iovec     — scatter/gather I/O vector
 *
 * 8. FLAG CONSTANTS (lines 147-172)
 *    access() mode bits, sigprocmask how-values, open() flags
 * ============================================================================
 */

/* ---------------------------------------------------------------------------
 * Fixed-width integer types.
 * All are guaranteed to be exactly the specified width on x86-64.
 * Use the explicitly-sized types (uint8_t / int32_t / uint64_t, etc.) for:
 *   - Hardware register definitions (PCI config space, MMIO, ACPI tables)
 *   - On-wire protocol headers (IP, TCP, UDP, ARP, DHCP, NBD, NVMe, AHCI)
 *   - File-system on-disk structures (FAT32, EXT2 superblocks/dir entries)
 *   - ABI-exposed structures (syscall arguments, signal frames, ptrace)
 * ------------------------------------------------------------------------ */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

/*
 * size_t  — used for object sizes, array indices, and byte counts.
 *           Always unsigned on this platform to match the address width.
 * ssize_t — signed counterpart; used when a negative error code must be
 *           returned alongside a byte count (e.g. read/write syscalls).
 * uintptr_t — integer type wide enough to hold any data pointer without
 *             truncation. Used for address arithmetic on virtual addresses.
 */
typedef uint64_t            size_t;
typedef int64_t             ssize_t;
typedef uint64_t            uintptr_t;

/* NULL pointer constant — guaranteed to compare unequal to any valid pointer */
#define NULL ((void *)0)
/* Boolean constants for use with the 'bool' type defined below */
#define true 1
#define false 0

/* ---------------------------------------------------------------------------
 * Compiler attribute macros
 *
 * These wrap common GCC/Clang __attribute__((...)) expressions into
 * short, readable names.  They control section placement, checking,
 * and optimisation hints without polluting the source with raw
 * attribute syntax.
 * ------------------------------------------------------------------------ */

/* __maybe_unused — suppress "unused function/variable" warning */
#define __maybe_unused __attribute__((unused))
/* __init — mark function as initialisation-only; discarded after boot */
#define __init        __attribute__((section(".init.text")))
/* __exit — module cleanup (empty — no separate exit section on x86) */
#define __exit        /* nothing — module cleanup, no exit section */
/* __printf — enable printf-format-string compile-time checks */
#define __printf(fmt, args) __attribute__((format(printf, fmt, args)))
/* __scanf  — enable scanf-format-string compile-time checks */
#define __scanf(fmt, args)  __attribute__((format(scanf, fmt, args)))
/* __nonnull — raise a warning when an annotated parameter receives NULL */
#define __nonnull     __attribute__((__nonnull__))

#ifndef __must_check
/* __must_check — warn if return value is discarded (e.g. error codes) */
#define __must_check        __attribute__((warn_unused_result))
#endif
#ifndef __malloc
/* __malloc — result is a freshly allocated pointer (non-aliased) */
#define __malloc            __attribute__((__malloc__))
#endif
#ifndef __read_mostly
/* __read_mostly — data that is frequently read, rarely written (friendly cacheline placement) */
#define __read_mostly       __attribute__((__section__(".data.read_mostly")))
#endif
#ifndef __cacheline_aligned
/* __cacheline_aligned — align on cache-line boundary (64 bytes on x86-64) to prevent false sharing */
#define __cacheline_aligned __attribute__((__aligned__(64)))
#endif
#ifndef __user
#ifdef __CHECKER__
/*
 * Sparse semantic parser annotations — enable address-space /
 * endianness / force-cast checking when running `make sparse`.
 *
 *   __user    — userspace address (address-space 1)
 *   __kernel  — kernel-space address (address-space 0, the default)
 *   __iomem   — MMIO / device memory address (address-space 2)
 *   __force   — suppress address-space warning on intentional casts
 *   __bitwise — endianness-checked integer type
 */
#define __user            __attribute__((noderef, address_space(1)))
#define __kernel          __attribute__((address_space(0)))
#define __iomem           __attribute__((noderef, address_space(2)))
#define __force           __attribute__((force))
#define __bitwise         __attribute__((bitwise))
#define __bitwise__       __attribute__((bitwise))
#else
#define __user            /* nothing — documentation only for userspace pointer annotations */
#define __kernel          /* nothing — kernel address space (default) */
#define __iomem           /* nothing — MMIO address space annotation */
#define __force           /* nothing — intentional address-space cast */
#define __bitwise         /* nothing — endianness annotation */
#define __bitwise__       /* nothing — endianness annotation */
#endif
#endif

#ifndef __no_sanitize_address
/* __no_sanitize_address — disable AddressSanitizer for functions that
 * intentionally access memory in ways ASan would flag (e.g. early boot
 * page-table manipulation, context-switch assembly wrappers). */
#define __no_sanitize_address __attribute__((__no_sanitize_address__))
#endif

/* ---------------------------------------------------------------------------
 * Branch prediction hints.
 *
 *   likely(x)   — tell the compiler x is true most of the time
 *   unlikely(x) — tell the compiler x is false most of the time
 *
 * These improve icache locality and pipeline utilisation by arranging the
 * common path as the fall-through branch.  Use sparingly — only on
 * hot-path conditions that are nearly deterministic at run time
 * (e.g. "is this a valid pointer?" after an allocation).
 * ------------------------------------------------------------------------ */
#ifndef likely
#define likely(x)     __builtin_expect(!!(x), 1)
#define unlikely(x)   __builtin_expect(!!(x), 0)
#endif

/* ---------------------------------------------------------------------------
 * Boolean type.
 *
 * C99 _Bool is a built-in unsigned integer type that can hold 0 or 1.
 * Assigning any non-zero value yields 1.  We typedef it to 'bool' for
 * readability and use the true/false macros above as constants.
 * ------------------------------------------------------------------------ */
typedef _Bool bool;

/* ---------------------------------------------------------------------------
 * Common constants and address helpers.
 * ------------------------------------------------------------------------ */

/* Maximum values for fixed-width integer types */
#define INT64_MAX  ((int64_t)9223372036854775807LL)
#define INT32_MAX  2147483647
#define UINT64_MAX ((uint64_t)-1)

/* Kernel virtual-memory area offset for the direct physical map.
 * On x86-64 the kernel is mapped in the upper half of the address space
 * (0xFFFF800000000000 and above).  Physical addresses are converted to
 * kernel virtual addresses by adding this offset. */
#define KERNEL_VMA_OFFSET  0xFFFF800000000000ULL
/* Convert between physical addresses and kernel virtual addresses.
 * Kernel uses a high-half VMA mapping: all physical accesses go through
 * KERNEL_VMA_OFFSET so the identity map can be removed. */
#define PHYS_TO_VIRT(addr) ((void *)((uint64_t)(addr) + KERNEL_VMA_OFFSET))
#define VIRT_TO_PHYS(addr) ((uint64_t)(uintptr_t)(addr) - KERNEL_VMA_OFFSET)

/* Page size for x86-64 — used by the MMU page tables, the page allocator,
 * and all I/O that operates in page-granularity chunks. */
#define PAGE_SIZE 4096

/* ---------------------------------------------------------------------------
 * fd_set — select() file-descriptor bitmask.
 *
 * Compatible with the POSIX.1-2001 select(2) API.  FD_SETSIZE limits the
 * maximum file descriptor number that can be monitored (default: 16).
 * The backing store is an array of 64-bit words sized to cover the range.
 * ------------------------------------------------------------------------ */

/* fd_set for select — FD_SETSIZE limits max monitored fd (default 16) */
#define FD_SETSIZE 16
typedef struct {
    uint64_t bits[(FD_SETSIZE + 63) / 64];
} fd_set;

/* FD_ZERO — clear all bits in the set */
static inline void FD_ZERO(fd_set *set) {
    for (int i = 0; i < (FD_SETSIZE + 63) / 64; i++) set->bits[i] = 0;
}
/* FD_SET — add fd to the set (bounded by FD_SETSIZE) */
static inline void FD_SET(int fd, fd_set *set) {
    if (fd >= 0 && fd < FD_SETSIZE) set->bits[fd / 64] |= (1ULL << (fd % 64));
}
/* FD_CLR — remove fd from the set (bounded by FD_SETSIZE) */
static inline void FD_CLR(int fd, fd_set *set) {
    if (fd >= 0 && fd < FD_SETSIZE) set->bits[fd / 64] &= ~(1ULL << (fd % 64));
}
/* FD_ISSET — test membership (returns 0 if fd out of range) */
static inline int FD_ISSET(int fd, fd_set *set) {
    if (fd < 0 || fd >= FD_SETSIZE) return 0;
    return (set->bits[fd / 64] >> (fd % 64)) & 1;
}

/* ---------------------------------------------------------------------------
 * POSIX structures for system calls and ABI compatibility.
 * ------------------------------------------------------------------------ */

/* timespec — nanosecond-resolution time for nanosleep / futex */
struct timespec {
    uint64_t tv_sec;
    uint64_t tv_nsec;
};

/* PAGE_SIZE must be 4096 for x86-64 page tables */
_Static_assert(PAGE_SIZE == 4096, "PAGE_SIZE must be 4096");

/* Compile-time ABI assertion: struct timespec must be 16 bytes */
_Static_assert(sizeof(struct timespec) == 16, "struct timespec size mismatch");

/* timeval — microsecond-resolution time for select / poll / setsockopt timeouts */
struct timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};

/* Compile-time ABI assertion: struct timeval must be 16 bytes */
_Static_assert(sizeof(struct timeval) == 16, "struct timeval size mismatch");

/* timex — NTP clock parameters (adjtimex(2) / ntp_adjtime(3)).
 * Layout mirrors the Linux x86_64 uapi struct timex. */
struct timex {
    uint32_t modes;      /* mode selector */
    int64_t offset;      /* time offset (us) */
    int64_t freq;        /* frequency offset (scaled ppm) */
    int64_t maxerror;    /* maximum error (us) */
    int64_t esterror;    /* estimated error (us) */
    int32_t status;      /* clock status */
    int64_t constant;    /* PLL time constant */
    int64_t precision;   /* clock precision (us) */
    int64_t tolerance;   /* clock frequency tolerance (scaled ppm) */
    struct timeval time; /* current time */
    int64_t tick;        /* microseconds per tick */
    int64_t ppsfreq;     /* PPS frequency (scaled ppm) */
    int64_t jitter;      /* PPS jitter (us) */
    int32_t shift;       /* PPS interval duration (s) */
    int64_t stabil;      /* PPS stability (scaled ppm) */
    int64_t jitcnt;      /* jitter limit exceeded count */
    int64_t calcnt;      /* calibration intervals */
    int64_t errcnt;      /* calibration errors */
    int64_t stbcnt;      /* stability limit exceeded count */
    int32_t tai;         /* TAI-UTC offset (s) */
};

/* adjtimex mode bits */
#define ADJ_OFFSET 0x0001
#define ADJ_FREQUENCY 0x0002
#define ADJ_MAXERROR 0x0004
#define ADJ_ESTERROR 0x0008
#define ADJ_STATUS 0x0010
#define ADJ_TIMECONST 0x0020
#define ADJ_TICK 0x4000
#define ADJ_TAI 0x0080

/* Clock status bits (Linux STA_* constants from <sys/timex.h>) */
#define STA_PLL 0x0001
#define STA_PPSFREQ 0x0002
#define STA_FLL 0x0008
#define STA_INS 0x0010 /* insert leap second */
#define STA_DEL 0x0020 /* delete leap second */
#define STA_UNSYNC 0x0040

/* utsname — system identification structure returned by uname(2) */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* iovec — scatter/gather I/O vector for readv(2) / writev(2) / preadv / pwritev */
struct iovec {
    void  *iov_base;
    uint64_t iov_len;
};

/* ---------------------------------------------------------------------------
 * POSIX flag constants for system calls.
 * ------------------------------------------------------------------------ */

/* access() mode constants: test for file existence / read / write / execute */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

/* sigprocmask how constants: SIG_BLOCK (mask additional signals),
 * SIG_UNBLOCK (unmask signals), SIG_SETMASK (set mask absolutely). */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* open() flag constants — bitmask values for the open(2) and openat(2) syscalls. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_NONBLOCK 04000
#define O_DSYNC 010000
#define O_SYNC 04010000
#define O_CLOEXEC 02000000
/* O_TMPFILE: create an unnamed temporary file (no directory entry).
 * Must not collide with O_CLOEXEC; we use bit 19 (0x80000).
 * Linux uses 0x200000 (bit 21), but our O_CLOEXEC is at bit 25. */
#define O_TMPFILE 0x80000

#endif
