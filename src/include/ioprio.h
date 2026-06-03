#ifndef IOPRIO_H
#define IOPRIO_H

#include "types.h"

/*
 * I/O Priority support (Item 327)
 *
 * Linux-compatible I/O priority classes and helpers.
 * Each process has an I/O priority that the block layer uses to
 * order requests: IOPRIO_CLASS_RT > IOPRIO_CLASS_BE > IOPRIO_CLASS_IDLE.
 *
 * Priority value encoding (Linux compatible):
 *   [0..2]  : I/O class (3 bits, but only 0-3 used)
 *   [3..7]  : priority data (within class, 0=highest, 7=lowest)
 *   [8..15] : reserved (must be zero)
 */

#define IOPRIO_CLASS_SHIFT      (13)
#define IOPRIO_PRIO_MASK        ((1UL << IOPRIO_CLASS_SHIFT) - 1)
#define IOPRIO_PRIO_CLASS_MASK  (7UL << IOPRIO_CLASS_SHIFT)

#define IOPRIO_PRIO_CLASS(ioprio)  (((ioprio) >> IOPRIO_CLASS_SHIFT) & 0x7)
#define IOPRIO_PRIO_DATA(ioprio)   ((ioprio) & IOPRIO_PRIO_MASK)
#define IOPRIO_PRIO_VALUE(class, data)  (((class) << IOPRIO_CLASS_SHIFT) | (data))

/* I/O priority classes */
#define IOPRIO_CLASS_NONE       0  /* No I/O priority set — use default */
#define IOPRIO_CLASS_RT         1  /* Real-time, highest priority */
#define IOPRIO_CLASS_BE         2  /* Best-effort, default class */
#define IOPRIO_CLASS_IDLE       3  /* Idle, only runs when no other I/O */

/* Default best-effort priority level (within class) */
#define IOPRIO_BE_NR           8  /* 8 priority levels in BE class */
#define IOPRIO_BE_DEF_PRIO     4  /* Default: middle of the range */

/* Default I/O priority for a new process */
#define IOPRIO_DEFAULT          IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, IOPRIO_BE_DEF_PRIO)

/* Syscall numbers (kernel-internal, not Linux-compatible on this kernel) */
#define SYS_IOPRIO_GET         555
#define SYS_IOPRIO_SET         556

/* ioprio_which identifiers (who argument for ioprio_get/ioprio_set) */
#define IOPRIO_WHO_PROCESS     1  /* by PID */
#define IOPRIO_WHO_PGRP        2  /* by process group */
#define IOPRIO_WHO_USER        3  /* by UID */

/* Return the numeric class priority for ordering requests.
 * Lower return value = higher priority.
 *   IOPRIO_CLASS_RT:  0..7      (map data 0=highest..7=lowest)
 *   IOPRIO_CLASS_BE:  8..15     (map data 0..7 → offset 8..15)
 *   IOPRIO_CLASS_IDLE: 16        (always lowest)
 *   IOPRIO_CLASS_NONE: treated as BE with default priority
 */
static inline uint8_t ioprio_class_order(uint16_t ioprio)
{
    unsigned int class = IOPRIO_PRIO_CLASS(ioprio);
    unsigned int data  = IOPRIO_PRIO_DATA(ioprio);

    switch (class) {
    case IOPRIO_CLASS_RT:
        /* RT data 0..7 → order 0..7 (clamp to 0..7) */
        if (data > 7) data = 7;
        return (uint8_t)data;
    case IOPRIO_CLASS_BE:
        /* BE data 0..7 → order 8..15 */
        if (data > 7) data = 7;
        return (uint8_t)(8 + data);
    case IOPRIO_CLASS_IDLE:
        return 16;  /* lowest */
    case IOPRIO_CLASS_NONE:
    default:
        /* Default: BE class, default priority */
        return (uint8_t)(8 + IOPRIO_BE_DEF_PRIO);
    }
}

#ifdef KERNEL_INTERNAL

/* Set the I/O priority for a process (internal helper) */
void sys_ioprio_set_process(struct process *proc, uint16_t ioprio);

#endif /* KERNEL_INTERNAL */

#endif /* IOPRIO_H */
