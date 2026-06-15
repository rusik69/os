#ifndef IO_URING_H
#define IO_URING_H

#include "types.h"
#include "process.h"
#include "vfs.h"

/* ── io_uring syscall numbers ───────────────────────────────────── */
#define SYS_IO_URING_SETUP    425
#define SYS_IO_URING_ENTER    426
#define SYS_IO_URING_REGISTER 427

/* ── io_uring parameters and flags ───────────────────────────────── */

/* io_uring_setup flags */
#define IORING_SETUP_IOPOLL        (1U << 0)
#define IORING_SETUP_SQPOLL        (1U << 1)
#define IORING_SETUP_SQ_AFF        (1U << 2)
#define IORING_SETUP_CQSIZE        (1U << 3)
#define IORING_SETUP_CLAMP         (1U << 4)
#define IORING_SETUP_ATTACH_WQ     (1U << 5)
#define IORING_SETUP_R_DISABLED    (1U << 6)
#define IORING_SETUP_SUBMIT_ALL    (1U << 7)
#define IORING_SETUP_COOP_TASKRUN  (1U << 8)
#define IORING_SETUP_TASKRUN_FLAG  (1U << 9)
#define IORING_SETUP_SQE128        (1U << 10)
#define IORING_SETUP_CQE32         (1U << 11)
#define IORING_REGISTER_FILES      0x02
/* io_uring_enter flags */
#define IORING_ENTER_GETEVENTS     (1U << 0)
#define IORING_ENTER_SQ_WAKEUP     (1U << 1)
#define IORING_ENTER_SQ_WAIT       (1U << 2)
#define IORING_ENTER_EXT_ARG       (1U << 3)
#define IORING_ENTER_REGISTERED_RING (1U << 4)

/* IORING_SETUP_ATTACH_WAIT — the feature requested in the task.
 * This is a flag passed in io_uring_params.flags during io_uring_setup.
 * When set, the kernel sets up the io_uring to allow io_uring_enter
 * to wait on completions from a parent ring (not yet supported fully). */

/* Operation codes (IORING_OP_*) */
#define IORING_OP_NOP              0
#define IORING_OP_READV            1
#define IORING_OP_WRITEV           2
#define IORING_OP_FSYNC            3
#define IORING_OP_READ             4
#define IORING_OP_WRITE            5
#define IORING_OP_POLL_ADD         6
#define IORING_OP_POLL_REMOVE      7
#define IORING_OP_SYNC_FILE_RANGE  8
#define IORING_OP_SENDMSG          9
#define IORING_OP_RECVMSG          10
#define IORING_OP_TIMEOUT          11
#define IORING_OP_TIMEOUT_REMOVE   12
#define IORING_OP_ACCEPT           13
#define IORING_OP_ASYNC_CANCEL     14
#define IORING_OP_LINK_TIMEOUT     15
#define IORING_OP_CONNECT          16
#define IORING_OP_FALLOCATE        17
#define IORING_OP_OPENAT           18
#define IORING_OP_CLOSE            19
#define IORING_OP_FILES_UPDATE     20
#define IORING_OP_STATX            21
#define IORING_OP_READ             22
#define IORING_OP_SPLICE           23
#define IORING_OP_PROVIDE_BUFFERS  24
#define IORING_OP_REMOVE_BUFFERS   25
#define IORING_OP_TEE              26
#define IORING_OP_SHUTDOWN         27
#define IORING_OP_RENAMEAT         28
#define IORING_OP_UNLINKAT         29
#define IORING_OP_MKDIRAT          30
#define IORING_OP_SYMLINKAT        31
#define IORING_OP_LINKAT           32

/* io_uring_register opcodes */
#define IORING_REGISTER_BUFFERS            0
#define IORING_UNREGISTER_BUFFERS          1
#define IORING_REGISTER_FILES              2
#define IORING_UNREGISTER_FILES            3
#define IORING_REGISTER_EVENTFD            4
#define IORING_UNREGISTER_EVENTFD          5
#define IORING_REGISTER_FILES_UPDATE       6
#define IORING_REGISTER_EVENTFD_ASYNC      7
#define IORING_REGISTER_PROBE              8
#define IORING_REGISTER_PERSONALITY        9
#define IORING_UNREGISTER_PERSONALITY      10
#define IORING_REGISTER_RESTRICTIONS       11
#define IORING_REGISTER_ENABLE_RINGS       12

/* SQEs submission flags */
#define IOSQE_FIXED_FILE_BIT    (1U << 0)  /* use registered file */
#define IOSQE_IO_DRAIN_BIT      (1U << 1)  /* issue after previous completes */
#define IOSQE_IO_LINK_BIT       (1U << 2)  /* link next SQE */
#define IOSQE_IO_HARDLINK       (1U << 3)  /* link regardless of result */
#define IOSQE_ASYNC_BIT         (1U << 4)  /* force async */
#define IOSQE_BUFFER_SELECT_BIT (1U << 5)  /* select buffer from group */

/* CQE flags */
#define IORING_CQE_F_BUFFER     (1U << 0)  /* buffer was selected */
#define IORING_CQE_F_MORE       (1U << 1)  /* more completions coming */
#define IORING_CQE_F_SOCK_NONEMPTY (1U << 2)

/* ── Data structures ─────────────────────────────────────────────── */

/* I/O vector for readv/writev — defined in types.h */

/* Submission Queue Entry (SQE) — 64 bytes */
struct io_uring_sqe {
    uint8_t   opcode;          /* IORING_OP_* */
    uint8_t   flags;           /* IOSQE_* bits */
    uint16_t  ioprio;          /* I/O priority */
    int32_t   fd;              /* file descriptor */
    union {
        uint64_t off;          /* offset into file */
        uint64_t addr2;
    };
    union {
        uint64_t addr;         /* pointer to buffer or iovec */
    };
    uint32_t  len;             /* buffer size / iov count */
    union {
        uint32_t  rw_flags;
        uint32_t  fsync_flags;
        uint16_t  poll_events;
        uint32_t  sync_range_flags;
        uint32_t  msg_flags;
        uint32_t  timeout_flags;
        uint32_t  accept_flags;
        uint32_t  cancel_flags;
        uint32_t  open_flags;
        uint32_t  statx_flags;
        uint32_t  fadvise_advice;
        uint32_t  splice_flags;
    };
    uint64_t  user_data;       /* data returned in CQE */
    union {
        uint16_t  buf_index;   /* buffer group to use */
        uint64_t  __pad2[3];
    };
};

/* Completion Queue Entry (CQE) — 16 bytes */
struct io_uring_cqe {
    uint64_t  user_data;       /* same as SQE user_data */
    int32_t   res;             /* result code (like syscall return) */
    uint32_t  flags;           /* IORING_CQE_F_* */
};

/* io_uring parameters (passed to io_uring_setup) */
struct io_uring_params {
    uint32_t  sq_entries;      /* submissions queue entries (filled by kernel) */
    uint32_t  cq_entries;      /* completions queue entries (filled by kernel) */
    uint32_t  flags;           /* IORING_SETUP_* flags */
    uint32_t  sq_thread_cpu;   /* CPU for SQ polling thread */
    uint32_t  sq_thread_idle;  /* idle time before thread exits */
    uint32_t  features;        /* feature flags (filled by kernel) */
    uint32_t  wq_fd;           /* fd of parent io_uring for IORING_SETUP_ATTACH_WQ */
    uint32_t  resv[3];         /* must be zero */
};

/* SQ ring offsets (returned in io_uring_params) - not used here */

/* ─── Per-ring state (kernel private) ─────────────────────────────── */

/* Maximum entries per ring */
#define IORING_MAX_ENTRIES  4096

/* A single io_uring instance */
struct io_ring {
    int          in_use;       /* 1 if allocated */

    /* Ring buffers (shared with userspace via mmap) */
    struct io_uring_sqe *sqes;          /* submission queue entries */
    uint32_t    *sq_array;              /* submission queue array (indexes into sqes) */
    uint32_t    *sq_head;               /* kernel: next to consume (shared) */
    uint32_t    *sq_tail;               /* userspace: next to submit (shared) */
    uint32_t    *sq_ring_mask;          /* ring size mask (shared) */
    uint32_t    *sq_ring_entries;       /* ring entries count (shared) */
    uint32_t     sq_flags;              /* shared flags */
    uint32_t    *sq_dropped;            /* dropped submissions count (shared) */
    uint32_t    *sq_array_size;         /* array size */

    struct io_uring_cqe *cqes;          /* completion queue entries */
    uint32_t    *cq_head;               /* userspace: next to consume (shared) */
    uint32_t    *cq_tail;               /* kernel: next to produce (shared) */
    uint32_t    *cq_ring_mask;          /* ring size mask (shared) */
    uint32_t    *cq_ring_entries;       /* ring entries count (shared) */
    uint32_t    *cq_overflow;           /* overflow count (shared) */

    uint32_t     entries;               /* number of slots */
    uint32_t     cq_entries;            /* completion ring entries */
    uint32_t     flags;                 /* IORING_SETUP_* flags */

    /* File descriptor of this ring (for reference) */
    int          ring_fd;

    /* PID of owner process */
    uint32_t     owner_pid;

    /* Ring pending completion queue (kernel-side) */
    uint32_t     cq_pending_head;
    uint32_t     cq_pending_tail;
    struct io_uring_cqe pending_cqes[IORING_MAX_ENTRIES];
};

/* ── Public API ────────────────────────────────────────────────────── */

/* Initialize the io_uring subsystem */
void io_uring_init(void);

/* sys_io_uring_setup — Create an io_uring instance.
 *   @entries:  requested number of SQ entries
 *   @params:   userspace pointer to io_uring_params
 *   Returns a file descriptor on success, or negative errno. */
int64_t sys_io_uring_setup(uint32_t entries, struct io_uring_params *params);

/* sys_io_uring_enter — Submit SQEs and/or wait for completions.
 *   @fd:            ring file descriptor
 *   @to_submit:     number of SQEs to submit
 *   @min_complete:  minimum number of completions to wait for
 *   @flags:         IORING_ENTER_* flags
 *   Returns number of SQEs submitted on success, or negative errno. */
int64_t sys_io_uring_enter(int fd, uint32_t to_submit, uint32_t min_complete,
                            uint32_t flags);

/* sys_io_uring_register — Register buffers or files for the ring.
 *   @fd:       ring file descriptor
 *   @opcode:   IORING_REGISTER_*
 *   @arg:      pointer to data
 *   @nr_args:  number of arguments
 *   Returns 0 on success, or negative errno. */
int64_t sys_io_uring_register(int fd, uint32_t opcode, void *arg,
                               uint32_t nr_args);

#endif /* IO_URING_H */
