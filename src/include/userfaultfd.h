#ifndef USERFAULTFD_H
#define USERFAULTFD_H

#include "types.h"
#include "spinlock.h"

#define UFFD_MAX_CONTEXTS 16
#define UFFD_EVENT_RING_SIZE 32

/* Maximum registered ranges per context */
#define UFFD_MAX_RANGES 16

/* Userfaultfd event types */
#define UFFD_EVENT_PAGEFAULT 1

/* Register/unregister mode flags */
#define UFFD_FLAG_MISSING    (1 << 0)  /* track missing page faults */
#define UFFD_FLAG_WP         (1 << 1)  /* track write-protect faults */
#define UFFD_FLAG_MINOR      (1 << 2)  /* track minor faults (page cache) */

/* Userfaultfd features (set via ioctl) */
#define UFFD_FEATURE_SIGBUS  (1 << 0)  /* deliver SIGBUS instead of queuing event */

/* ── UFFDIO ioctl commands (Linux-compatible values) ──────────────── */
#define UFFDIO_API          0x3F00  /* negotiate API version and features */
#define UFFDIO_REGISTER     0x3F01  /* register a memory range */
#define UFFDIO_UNREGISTER   0x3F02  /* unregister a memory range */
#define UFFDIO_COPY         0x3F03  /* copy a page from userspace */
#define UFFDIO_ZEROPAGE     0x3F04  /* map zero page */
#define UFFDIO_WAKE         0x3F05  /* wake blocked thread */

/* UFFDIO_API ioctl argument */
struct uffdio_api {
    uint64_t api;          /* request/response: UFFD_API */
    uint64_t features;     /* request: desired features; response: available features */
    uint64_t ioctls;       /* response: bitmask of supported ioctls */
};

/* UFFDIO_REGISTER ioctl argument */
struct uffdio_register {
    uint64_t range_start;  /* start of range (page-aligned) */
    uint64_t range_len;    /* length (must be multiple of page size) */
    uint64_t mode;         /* UFFD_FLAG_* bits */
    uint64_t ioctls;       /* response: bitmask of supported ioctls for registered range */
};

/* UFFDIO_COPY ioctl argument */
struct uffdio_copy {
    uint64_t dst;          /* destination address (userspace, must be page-aligned) */
    uint64_t src;          /* source address (userspace buffer) */
    uint64_t len;          /* length (must be page-aligned, currently 1 page) */
    uint64_t mode;         /* flags: UFFDIO_COPY_MODE_DONTWAKE (bit 0) */
    int64_t  copy;         /* response: number of bytes copied, or negative errno */
};

/* UFFDIO_ZEROPAGE ioctl argument */
struct uffdio_zeropage {
    uint64_t range_start;  /* start of range (page-aligned) */
    uint64_t range_len;    /* length (must be page-aligned) */
    uint64_t mode;         /* flags: UFFDIO_ZEROPAGE_MODE_DONTWAKE (bit 0) */
    int64_t  zeropage;     /* response: number of bytes zeroed, or negative errno */
};

/* UFFDIO_WAKE ioctl argument */
struct uffdio_wake {
    uint64_t range_start;  /* start of range (page-aligned) */
    uint64_t range_len;    /* length (must be page-aligned) */
    uint64_t mode;         /* reserved, must be 0 */
};

/* UFFDIO_COPY / ZEROPAGE mode flags */
#define UFFDIO_COPY_MODE_DONTWAKE     (1 << 0)
#define UFFDIO_ZEROPAGE_MODE_DONTWAKE (1 << 0)

/* UFFD_API version */
#define UFFD_API 0xAA

/* Registered range entry */
struct uffd_range {
    uint64_t start;         /* start virtual address */
    uint64_t end;           /* end virtual address (exclusive) */
    int      mode;          /* UFFD_FLAG_MISSING | UFFD_FLAG_WP | UFFD_FLAG_MINOR */
    int      in_use;
};

struct uffd_event {
    uint64_t fault_addr;
    uint64_t fault_flags; /* bit 0: write fault, bit 1: minor fault */
    int pending;
};

struct uffd_context {
    spinlock_t lock;
    int used;
    int fd;              /* index (same as array index) */

    /* Registered address ranges */
    struct uffd_range ranges[UFFD_MAX_RANGES];
    int num_ranges;

    /* Event ring buffer */
    struct uffd_event events[UFFD_EVENT_RING_SIZE];
    int event_head;
    int event_tail;

    /* Feature flags (set via UFFDIO_API ioctl) */
    uint64_t features;
};

/* Create a new userfaultfd context.  Returns fd index or -errno. */
int userfaultfd_create(int flags);

/* Register/unregister a virtual memory range with a userfaultfd context. */
int userfaultfd_register(int fd, uint64_t addr, uint64_t len, int mode);
int userfaultfd_unregister(int fd, uint64_t addr, uint64_t len);

/* Handle a page fault: if a registered range covers fault_addr, queue an event
 * or signal SIGBUS depending on feature flags.  Returns 0 if handled
 * (event queued or SIGBUS sent), 1 if not handled (not a uffd range),
 * or -errno on error. */
int userfaultfd_handle_fault(int fd, uint64_t fault_addr, int write, int is_minor);

/* Find which userfaultfd context (if any) handles the given fault address
 * for the current process.  Returns fd index if found, -1 if none. */
int userfaultfd_find_for_addr(uint64_t fault_addr, int *write_flag);

/* Read queued fault events from the ring buffer into user-supplied buffer. */
int64_t userfaultfd_read(int fd, void *buf, uint64_t count);

/* Set features on a userfaultfd context (called from UFFDIO_API ioctl). */
int userfaultfd_set_features(int fd, uint64_t features);

/* UFFDIO operations — called from the userfaultfd syscall handler. */
int userfaultfd_copy(int fd, struct uffdio_copy *copy_arg);
int userfaultfd_zeropage(int fd, struct uffdio_zeropage *zp_arg);
int userfaultfd_wake(int fd, struct uffdio_wake *wake_arg);
int userfaultfd_api(int fd, struct uffdio_api *api_arg);

/* Syscall entry point: cmd and arg are passed from the userfaultfd syscall.
 * Returns fd for UFFDIO_API (create case), or 0 on success, or -errno. */
int64_t sys_userfaultfd(uint64_t cmd, uint64_t arg);

/* Unified userfaultfd syscall: fd, cmd, arg. */
int64_t sys_userfaultfd2(uint64_t fd, uint64_t cmd, uint64_t arg);

/* Initialise the userfaultfd subsystem. */
void uffd_init(void);

#endif /* USERFAULTFD_H */
