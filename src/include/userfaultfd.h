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

/* Read queued fault events from the ring buffer into user-supplied buffer. */
int64_t userfaultfd_read(int fd, void *buf, uint64_t count);

/* Set features on a userfaultfd context (called from UFFDIO_API ioctl). */
int userfaultfd_set_features(int fd, uint64_t features);

/* Initialise the userfaultfd subsystem. */
void uffd_init(void);

#endif /* USERFAULTFD_H */
