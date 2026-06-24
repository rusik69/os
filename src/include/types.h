#ifndef TYPES_H
#define TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef uint64_t            size_t;
typedef int64_t             ssize_t;
typedef uint64_t            uintptr_t;

#define NULL ((void *)0)
#define true 1
#define false 0

#define __maybe_unused __attribute__((unused))
#define __init        __attribute__((section(".init.text")))
#define __printf(fmt, args) __attribute__((format(printf, fmt, args)))

#ifndef __must_check
#define __must_check        __attribute__((warn_unused_result))
#endif
#ifndef __malloc
#define __malloc            __attribute__((__malloc__))
#endif
#ifndef __read_mostly
#define __read_mostly       __attribute__((__section__(".data.read_mostly")))
#endif
#ifndef __cacheline_aligned
#define __cacheline_aligned __attribute__((__aligned__(64)))
#endif
#ifndef __user
#define __user            /* nothing — documentation only for userspace pointer annotations */
#endif

#ifndef __no_sanitize_address
#define __no_sanitize_address __attribute__((__no_sanitize_address__))
#endif

#ifndef likely
#define likely(x)     __builtin_expect(!!(x), 1)
#define unlikely(x)   __builtin_expect(!!(x), 0)
#endif

typedef _Bool bool;

#define INT64_MAX  ((int64_t)9223372036854775807LL)
#define INT32_MAX  2147483647
#define UINT64_MAX ((uint64_t)-1)

#define KERNEL_VMA_OFFSET  0xFFFF800000000000ULL
/* Convert between physical addresses and kernel virtual addresses.
 * Kernel uses a high-half VMA mapping: all physical accesses go through
 * KERNEL_VMA_OFFSET so the identity map can be removed. */
#define PHYS_TO_VIRT(addr) ((void *)((uint64_t)(addr) + KERNEL_VMA_OFFSET))
#define VIRT_TO_PHYS(addr) ((uint64_t)(uintptr_t)(addr) - KERNEL_VMA_OFFSET)

#define PAGE_SIZE 4096

/* fd_set for select */
#define FD_SETSIZE 16
typedef struct {
    uint64_t bits[(FD_SETSIZE + 63) / 64];
} fd_set;

static inline void FD_ZERO(fd_set *set) {
    for (int i = 0; i < (FD_SETSIZE + 63) / 64; i++) set->bits[i] = 0;
}
static inline void FD_SET(int fd, fd_set *set) {
    if (fd >= 0 && fd < FD_SETSIZE) set->bits[fd / 64] |= (1ULL << (fd % 64));
}
static inline void FD_CLR(int fd, fd_set *set) {
    if (fd >= 0 && fd < FD_SETSIZE) set->bits[fd / 64] &= ~(1ULL << (fd % 64));
}
static inline int FD_ISSET(int fd, fd_set *set) {
    if (fd < 0 || fd >= FD_SETSIZE) return 0;
    return (set->bits[fd / 64] >> (fd % 64)) & 1;
}

/* timespec for nanosleep */
struct timespec {
    uint64_t tv_sec;
    uint64_t tv_nsec;
};

/* PAGE_SIZE must be 4096 for x86-64 page tables */
_Static_assert(PAGE_SIZE == 4096, "PAGE_SIZE must be 4096");

/* Compile-time ABI assertion: struct timespec must be 16 bytes */
_Static_assert(sizeof(struct timespec) == 16, "struct timespec size mismatch");

/* timeval for select/poll/setsockopt timeouts */
struct timeval {
    uint64_t tv_sec;
    uint64_t tv_usec;
};

/* Compile-time ABI assertion: struct timeval must be 16 bytes */
_Static_assert(sizeof(struct timeval) == 16, "struct timeval size mismatch");

/* utsname for uname */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* iovec for readv/writev */
struct iovec {
    void  *iov_base;
    uint64_t iov_len;
};

/* access() mode constants */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

/* sigprocmask how constants */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* open() flag constants */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000
/* O_TMPFILE: create an unnamed temporary file (no directory entry).
 * Must not collide with O_CLOEXEC; we use bit 19 (0x80000).
 * Linux uses 0x200000 (bit 21), but our O_CLOEXEC is at bit 25. */
#define O_TMPFILE 0x80000

#endif
