/*
 * sys/mman.h — Memory-mapped file and anonymous memory interface
 *
 * This header defines the constants, types, and function prototypes for
 * mapping files or anonymous pages into a process's virtual address space.
 * The mmap() family of calls is the primary mechanism for:
 *
 *   • File-backed memory mapping (demand-paged from a file descriptor)
 *   • Anonymous memory allocation (MAP_ANONYMOUS, replaces sbrk/brk)
 *   • Shared memory between processes (MAP_SHARED on same file)
 *   • Fixed-address mapping (MAP_FIXED, for JITs, bootloaders, etc.)
 *   • Huge page allocation (MAP_HUGETLB, for performance-critical regions)
 *
 * The API follows POSIX.1-2017 (mmap, munmap, mprotect, msync, mlock,
 * munlock, madvise) with additional Linux extensions.
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │     PROT_* flags (protection bits, OR-able)                     │
 * ├──────────────┬────────┬─────────────────────────────────────────┤
 * │  Constant    │ Value  │  Effect                                 │
 * ├──────────────┼────────┼─────────────────────────────────────────┤
 * │  PROT_NONE   │  0x00  │  Page is not accessible at all           │
 * │  PROT_READ   │  0x01  │  Page contents may be read               │
 * │  PROT_WRITE  │  0x02  │  Page contents may be written            │
 * │  PROT_EXEC   │  0x04  │  Page contents may be executed           │
 * │  PROT_SAO    │  0x10  │  Strong access ordering (PowerPC)       │
 * │  PROT_GROWSDOWN │0x01000000 │ Apply protection to growing stack  │
 * │  PROT_GROWSUP   │0x02000000 │ Apply protection to growing upward │
 * └──────────────┴────────┴─────────────────────────────────────────┘
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │     MAP_* flags (mapping behavior, OR-able)                     │
 * ├─────────────────┬──────────┬────────────────────────────────────┤
 * │  Constant       │  Value   │  Effect                            │
 * ├─────────────────┼──────────┼────────────────────────────────────┤
 * │  MAP_SHARED     │  0x01    │  Share changes with other mappings │
 * │  MAP_PRIVATE    │  0x02    │  Copy-on-write private mapping     │
 * │  MAP_SHARED_VALIDATE │0x03 │  Like MAP_SHARED, validates flags  │
 * │  MAP_FIXED      │  0x10    │  Interpret addr literally          │
 * │  MAP_ANONYMOUS  │  0x20    │  Not backed by a file              │
 * │  MAP_32BIT      │  0x40    │  Map in first 2 GB of address space│
 * │  MAP_GROWSDOWN  │  0x0100  │  Stack-like auto-growing mapping   │
 * │  MAP_DENYWRITE  │  0x0800  │  Block writes to underlying file   │
 * │  MAP_EXECUTABLE │  0x1000  │  Mark mapping as executable (hist.)│
 * │  MAP_LOCKED     │  0x2000  │  Lock pages in memory (see mlock)  │
 * │  MAP_NORESERVE  │  0x4000  │  Don't reserve swap space          │
 * │  MAP_POPULATE   │  0x8000  │  Pre-fault pages (readahead)       │
 * │  MAP_UNINITIALIZED │0x4000000 │ Don't clear anonymous pages     │
 * │  MAP_HUGETLB    │ 0x40000  │  Use huge pages for mapping        │
 * │  MAP_STACK      │ 0x200000 │  Mapping is a stack (guard page)   │
 * │  MAP_SYNC       │ 0x80000  │  Synchronous page faults (DAX)     │
 * │  MAP_FIXED_NOREPLACE │0x100000 │ Like MAP_FIXED but fails if   │
 * │                 │          │  something is already mapped       │
 * └─────────────────┴──────────┴────────────────────────────────────┘
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │     MAP_HUGETLB flags (huge-page size encoding, bits 26-31)     │
 * ├──────────────────────────────────────────────────────────────────┤
 * │  The huge-page size is encoded in the MAP_HUGETLB bits:         │
 * │  (MAP_HUGETLB | (shift << MAP_HUGE_SHIFT)), where shift is the  │
 * │  log2 of the desired page size (e.g., 21 for 2 MB, 30 for 1 GB).│
 * │                                                                  │
 * │  MAP_HUGE_SHIFT = 26                                            │
 * │  MAP_HUGE_2MB   = 21 << MAP_HUGE_SHIFT                          │
 * │  MAP_HUGE_1GB   = 30 << MAP_HUGE_SHIFT                          │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │     MSYNC flags                                                  │
 * ├─────────────┬────────┬──────────────────────────────────────────┤
 * │  MS_ASYNC   │  0x01  │  Schedule writeback; return immediately  │
 * │  MS_SYNC    │  0x04  │  Wait for writeback to complete          │
 * │  MS_INVALIDATE │0x02  │  Invalidate cached copies (coherency)   │
 * └─────────────┴────────┴──────────────────────────────────────────┘
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │     MLOCK / MUNLOCK flags                                        │
 * ├──────────────┬────────┬─────────────────────────────────────────┤
 * │  MCL_CURRENT  │  0x01  │  Lock all currently mapped pages       │
 * │  MCL_FUTURE   │  0x02  │  Lock all future mappings              │
 * │  MCL_ONFAULT  │  0x04  │  Lock pages only when they are faulted │
 * └──────────────┴────────┴─────────────────────────────────────────┘
 *
 * Reference: POSIX.1-2017 <sys/mman.h>, Linux mmap(2) man-page,
 *            and kernel Documentation/admin-guide/mm/hugetlbpage.rst.
 */

#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stdint.h>

/* ================================================================= */
/*  Protection flags — used for both mmap() and mprotect()           */
/* ================================================================= */

#define PROT_NONE       0x00            /* No access at all                */
#define PROT_READ       0x01            /* Pages can be read              */
#define PROT_WRITE      0x02            /* Pages can be written           */
#define PROT_EXEC       0x04            /* Pages can be executed          */

/* Additional protection flags (non-POSIX, Linux-specific) */
#define PROT_SAO        0x10            /* Strong access ordering (PPC)   */
#define PROT_GROWSDOWN  0x01000000      /* Apply to growing segment down  */
#define PROT_GROWSUP    0x02000000      /* Apply to growing segment up    */

/* ================================================================= */
/*  Mapping flags — control sharing, location, and behaviour         */
/* ================================================================= */

/* Sharing type (exactly one of MAP_SHARED, MAP_PRIVATE, or
 * MAP_SHARED_VALIDATE must be specified) */
#define MAP_SHARED          0x01        /* Share writes with other mappers */
#define MAP_PRIVATE         0x02        /* Copy-on-write private snapshot  */
#define MAP_SHARED_VALIDATE 0x03        /* MAP_SHARED + flag validation    */

/* Address interpretation */
#define MAP_FIXED           0x10        /* Exact address, fail if occupied */
#define MAP_FIXED_NOREPLACE 0x100000    /* Like MAP_FIXED but safe         */

/* Content / backing */
#define MAP_ANONYMOUS       0x20        /* Not backed by any file          */
#define MAP_32BIT           0x40        /* Map into first 2 GiB of address */

/* Behaviour modifiers */
#define MAP_GROWSDOWN       0x0100      /* Stack-like auto-growth          */
#define MAP_DENYWRITE       0x0800      /* Block writes to backing file    */
#define MAP_EXECUTABLE      0x1000      /* (historical, no-op)             */
#define MAP_LOCKED          0x2000      /* Lock pages in RAM               */
#define MAP_NORESERVE       0x4000      /* Don't reserve swap space        */
#define MAP_POPULATE        0x8000      /* Pre-populate page tables        */
#define MAP_STACK           0x200000    /* Mapping is a stack (guard pge)  */
#define MAP_HUGETLB         0x40000     /* Use huge pages                  */
#define MAP_SYNC            0x80000     /* Synchronous page faults (DAX)   */
#define MAP_UNINITIALIZED   0x4000000   /* Don't zero anonymous pages      */

/* Huge-page size encoding (used with MAP_HUGETLB) */
#define MAP_HUGE_SHIFT      26          /* Shift for huge-page size encode */
#define MAP_HUGE_2MB        (21 << MAP_HUGE_SHIFT)  /* 2 MiB huge pages   */
#define MAP_HUGE_1GB        (30 << MAP_HUGE_SHIFT)  /* 1 GiB huge pages   */

/* ================================================================= */
/*  Error sentinel                                                   */
/* ================================================================= */

#define MAP_FAILED          ((void *) -1)   /* mmap() returns this on err */

/* ================================================================= */
/*  msync() flags                                                    */
/* ================================================================= */

#define MS_ASYNC            0x01        /* Schedule, return immediately    */
#define MS_SYNC             0x04        /* Synchronous writeback          */
#define MS_INVALIDATE       0x02        /* Invalidate cached copies       */

/* ================================================================= */
/*  mlockall() / munlockall() flags                                   */
/* ================================================================= */

#define MCL_CURRENT         0x01        /* Lock all current mappings      */
#define MCL_FUTURE          0x02        /* Lock all future mappings       */
#define MCL_ONFAULT         0x04        /* Lock only on page fault        */

/* ================================================================= */
/*  madvise() advice values                                           */
/* ================================================================= */

#define MADV_NORMAL         0           /* No special treatment           */
#define MADV_RANDOM         1           /* Expect random page references  */
#define MADV_SEQUENTIAL     2           /* Expect sequential references   */
#define MADV_WILLNEED       3           /* Will need these pages soon     */
#define MADV_DONTNEED       4           /* Don't need these pages anymore */
#define MADV_FREE           8           /* Free pages (lazy, like FALLOC) */
#define MADV_REMOVE         9           /* Remove backing store pages     */
#define MADV_DONTFORK       10          /* Don't inherit across fork      */
#define MADV_DOFORK         11          /* Do inherit across fork         */
#define MADV_MERGEABLE      12          /* KSM may merge anonymous pages  */
#define MADV_UNMERGEABLE    13          /* Revert MADV_MERGEABLE          */
#define MADV_HUGEPAGE       14          /* Use huge pages                 */
#define MADV_NOHUGEPAGE     15          /* Exclude from huge-page range   */
#define MADV_DONTDUMP       16          /* Skip in core dump              */
#define MADV_DODUMP         17          /* Include in core dump           */
#define MADV_WIPEONFORK     18          /* Zero memory in child after fork*/
#define MADV_KEEPONFORK     19          /* Preserve memory after fork     */
#define MADV_COLD           20          /* Deactivate pages (cold)        */
#define MADV_PAGEOUT        21          /* Reclaim pages immediately      */
#define MADV_POPULATE_READ  22          /* Populate (prefault) pages ro   */
#define MADV_POPULATE_WRITE 23          /* Populate (prefault) pages rw   */
#define MADV_DONTNEED_LOCKED 24         /* Like DONTNEED but locked       */

/* ================================================================= */
/*  Function prototypes                                               */
/* ================================================================= */

/*
 * mmap() — Create a memory mapping.
 *
 * @addr:  Hint for the starting address (or exact if MAP_FIXED).
 * @length: Number of bytes to map.
 * @prot:  Protection flags (PROT_READ | PROT_WRITE | PROT_EXEC | ...).
 * @flags: Mapping type and behaviour (MAP_SHARED / MAP_PRIVATE | ...).
 * @fd:    File descriptor (ignored if MAP_ANONYMOUS is set).
 * @offset: File offset, must be page-aligned.
 *
 * Returns: On success, the mapped address. On error, MAP_FAILED.
 */
void *mmap(void *addr, uintptr_t length, int prot, int flags, int fd,
           intptr_t offset);

/*
 * munmap() — Unmap a previously mapped region.
 *
 * @addr:  Address of the region (must be page-aligned).
 * @length: Number of bytes to unmap.
 *
 * Returns: 0 on success, -1 on error.
 */
int munmap(void *addr, uintptr_t length);

/*
 * mprotect() — Change protection on a mapped region.
 *
 * @addr:  Start address (must be page-aligned).
 * @length: Number of bytes.
 * @prot:  New protection flags.
 *
 * Returns: 0 on success, -1 on error.
 */
int mprotect(void *addr, uintptr_t length, int prot);

/*
 * msync() — Synchronise a mapped region with the backing file.
 *
 * @addr:   Start address (must be page-aligned).
 * @length: Number of bytes.
 * @flags:  MS_ASYNC, MS_SYNC, or MS_INVALIDATE (may be OR-ed).
 *
 * Returns: 0 on success, -1 on error.
 */
int msync(void *addr, uintptr_t length, int flags);

/*
 * mlock() — Lock pages in memory (prevent swapping).
 *
 * @addr:   Start address (must be page-aligned).
 * @length: Number of bytes.
 *
 * Returns: 0 on success, -1 on error.
 */
int mlock(const void *addr, uintptr_t length);

/*
 * munlock() — Unlock pages previously locked with mlock().
 *
 * @addr:   Start address (must be page-aligned).
 * @length: Number of bytes.
 *
 * Returns: 0 on success, -1 on error.
 */
int munlock(const void *addr, uintptr_t length);

/*
 * mlockall() — Lock all pages mapped by the calling process.
 *
 * @flags: MCL_CURRENT, MCL_FUTURE, MCL_ONFAULT (may be OR-ed).
 *
 * Returns: 0 on success, -1 on error.
 */
int mlockall(int flags);

/*
 * munlockall() — Undo a previous mlockall() call.
 *
 * Returns: 0 on success, -1 on error.
 */
int munlockall(void);

/*
 * madvise() — Give advice about page usage patterns.
 *
 * @addr:    Start address (must be page-aligned).
 * @length:  Number of bytes.
 * @advice:  One of the MADV_* constants.
 *
 * Returns: 0 on success, -1 on error.
 */
int madvise(void *addr, uintptr_t length, int advice);

#endif /* _SYS_MMAN_H */
