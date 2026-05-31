/*
 * eventfd — event notification file descriptor
 *
 * Creates a counter-based file descriptor for event notification.
 * Supports EFD_SEMAPHORE, EFD_NONBLOCK, EFD_CLOEXEC.
 * Counter is an unsigned 64-bit value.
 *
 * fd range: 700..715 (max 16 eventfds)
 */
#include "eventfd.h"
#include "process.h"
#include "printf.h"
#include "string.h"
#include "scheduler.h"
#include "waitqueue.h"

#define EVENTFD_MAX 16
#define EVENTFD_BASE 700

struct eventfd_info {
    int      in_use;
    uint64_t counter;          /* the eventfd counter */
    int      flags;            /* EFD_SEMAPHORE, EFD_NONBLOCK, EFD_CLOEXEC */
    struct wait_queue wq;      /* waiters */
};

static struct eventfd_info eventfd_table[EVENTFD_MAX];

/* Initialize an eventfd slot */
static int eventfd_alloc(uint32_t initval, int flags) {
    for (int i = 0; i < EVENTFD_MAX; i++) {
        if (!eventfd_table[i].in_use) {
            eventfd_table[i].in_use = 1;
            eventfd_table[i].counter = initval;
            eventfd_table[i].flags = flags;
            wait_queue_init(&eventfd_table[i].wq);
            return EVENTFD_BASE + i;
        }
    }
    return -1;
}

static struct eventfd_info *eventfd_get(int fd) {
    int slot = fd - EVENTFD_BASE;
    if (slot < 0 || slot >= EVENTFD_MAX) return NULL;
    if (!eventfd_table[slot].in_use) return NULL;
    return &eventfd_table[slot];
}

/* ── Public API ─────────────────────────────────────────────────────── */

int eventfd_create(uint32_t initval, int flags) {
    int fd = eventfd_alloc(initval, flags);
    if (fd < 0) return -1;
    return fd;
}

int eventfd_read(int fd, uint64_t *val) {
    struct eventfd_info *efd = eventfd_get(fd);
    if (!efd) return -1;

    for (;;) {
        if (efd->counter > 0) {
            if (efd->flags & EFD_SEMAPHORE) {
                /* Decrement by 1, return 1 */
                efd->counter--;
                *val = 1;
            } else {
                /* Return full counter value, reset to 0 */
                *val = efd->counter;
                efd->counter = 0;
            }
            return 0;
        }

        /* Counter is 0 — need to wait or return EAGAIN */
        if (efd->flags & EFD_NONBLOCK) {
            return -1; /* EAGAIN */
        }

        /* Block until write happens */
        wait_queue_sleep(&efd->wq);
    }
}

int eventfd_write(int fd, uint64_t val) {
    struct eventfd_info *efd = eventfd_get(fd);
    if (!efd) return -1;

    if (val == 0xFFFFFFFFFFFFFFFFULL) {
        return -1; /* EINVAL */
    }

    uint64_t new_val = efd->counter + val;

    /* Check for overflow */
    if (new_val < efd->counter) {
        /* Overflow — would block. Non-blocking: return EAGAIN */
        if (efd->flags & EFD_NONBLOCK) {
            return -1;
        }
        /* Blocking: should wait, but for simplicity just clamp to UINT64_MAX */
        new_val = 0xFFFFFFFFFFFFFFFFULL;
    }

    efd->counter = new_val;

    /* Wake all waiters */
    wait_queue_wake_all(&efd->wq);

    return 0;
}

void eventfd_close(int fd) {
    struct eventfd_info *efd = eventfd_get(fd);
    if (!efd) return;
    efd->in_use = 0;
    efd->counter = 0;
}

/* ── Syscall wrappers called from syscall dispatch ──────────────────── */

int eventfd_syscall(uint32_t initval, int flags) {
    return eventfd_create(initval, flags);
}
