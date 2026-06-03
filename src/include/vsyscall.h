#ifndef VSYSCALL_H
#define VSYSCALL_H

#include "types.h"

/*
 * vsyscall.h — vDSO (virtual dynamic shared object) for userspace
 *
 * Maps a read-only data page + code page into every user process so
 * that certain system calls (gettimeofday, clock_gettime, time) can
 * be satisfied entirely in userspace by reading the Time-Stamp Counter
 * (TSC) and applying calibration data from the kernel.
 *
 * This avoids the overhead of a ring transition for these frequently
 * called time functions.
 */

/* Fixed virtual address for the vDSO (user-accessible, top of user VA) */
#define VSYSCALL_PAGE_VADDR  0xFFFFFFFFFF600000ULL

/* vDSO data page virtual address (immediately after the code page) */
#define VDSO_DATA_PAGE_VADDR 0xFFFFFFFFFF601000ULL

/* Number of vDSO function entries */
#define VSYSCALL_NR_ENTRIES 4

/* vDSO entry indices */
#define VSYSCALL_GETTIMEOFDAY 0  /* int gettimeofday(struct timeval *tv, struct timezone *tz) */
#define VSYSCALL_TIME         1  /* time_t time(time_t *t) */
#define VSYSCALL_GETCPU       2  /* int getcpu(unsigned *cpu, unsigned *node) */
#define VSYSCALL_CLOCK_GETTIME 3 /* int clock_gettime(clockid_t clk_id, struct timespec *tp) */

/*
 * vDSO clock data — shared read-only page between kernel and userspace.
 *
 * The kernel updates this structure on every timer tick.  Userspace code
 * in the vDSO page reads it along with the TSC to compute the current
 * time without a syscall.
 *
 * A seqlock (sequence counter) protects against torn reads: userspace
 * reads the sequence before and after; if they differ (odd = writer
 * active) or mismatch (concurrent write), retry.
 */
struct vdso_clock_data {
    volatile uint64_t seq;            /* Seqlock: odd = writer active */

    /* CLOCK_REALTIME + TSC correlation */
    uint64_t tsc_timestamp;           /* TSC value at last update */
    uint64_t wall_sec;                /* Wall clock (REALTIME) seconds */
    uint64_t wall_nsec;               /* Wall clock nanoseconds [0, 1e9) */
    uint64_t tsc_to_ns_mult;          /* (tsc_delta * mult) >> shift = nsec */
    uint8_t  tsc_shift;              /* Shift for the multiply */
    uint64_t tsc_max_delta;           /* Max delta before overflow */

    /* CLOCK_MONOTONIC correlation */
    uint64_t mono_sec;                /* Monotonic seconds */
    uint64_t mono_nsec;               /* Monotonic nanoseconds [0, 1e9) */

    /* Padding to fill a full page (4K) */
    uint8_t  __pad[4031];
} __attribute__((packed));

/* Ensure the structure fits in one page */
_Static_assert(sizeof(struct vdso_clock_data) <= 4096,
               "vdso_clock_data must fit in 4K page");

/*
 * Kernel-side initialisation and update.
 */
int  vsyscall_init(void);
void vsyscall_update_clock(void);

/* Get the vDSO page virtual address (for mapping into user processes) */
void *vsyscall_get_page(void);

/* Get the vDSO data page physical address */
uint64_t vsyscall_get_data_phys(void);

#endif /* VSYSCALL_H */
