#ifndef USERFAULTFD_H
#define USERFAULTFD_H

#include "types.h"
#include "spinlock.h"

#define UFFD_MAX_CONTEXTS 16
#define UFFD_EVENT_RING_SIZE 32

/* Userfaultfd event types */
#define UFFD_EVENT_PAGEFAULT 1

/* Register/unregister mode flags */
#define UFFD_MODE_MISSING 1
#define UFFD_MODE_WP      2

struct uffd_event {
    uint64_t fault_addr;
    uint64_t fault_flags; /* bit 0: write fault */
    int pending;
};

struct uffd_context {
    spinlock_t lock;
    int used;
    int fd;              /* index (same as array index) */
    struct uffd_event events[UFFD_EVENT_RING_SIZE];
    int event_head;
    int event_tail;
};

/* Create a new userfaultfd context.  Returns fd index or -errno. */
int userfaultfd_create(int flags);

/* Register/unregister a virtual memory range with a userfaultfd context. */
int userfaultfd_register(int fd, uint64_t addr, uint64_t len, int mode);
int userfaultfd_unregister(int fd, uint64_t addr, uint64_t len);

/* Handle a page fault: if a registered range covers fault_addr, queue an event. */
int userfaultfd_handle_fault(int fd, uint64_t fault_addr, int write);

/* Read queued fault events from the ring buffer into user-supplied buffer. */
int64_t userfaultfd_read(int fd, void *buf, uint64_t count);

/* Initialise the userfaultfd subsystem. */
void uffd_init(void);

#endif /* USERFAULTFD_H */
