#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "waitqueue.h"
#include "vfs.h"
#include "socket.h"

/* ── signalfd: receive signals as file descriptor ────────────────────── */

#define SIGNALFD_MAX 16

struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
} __attribute__((packed));

struct signalfd_entry {
    int in_use;
    uint32_t pid;
    uint64_t sigmask;    /* signals to capture */
    uint8_t  queue[256]; /* pending signal data */
    int      queue_len;
    struct wait_queue wq;
};

static struct signalfd_entry signalfd_table[SIGNALFD_MAX];
static int signalfd_initialized = 0;

void signalfd_init(void) {
    if (signalfd_initialized) return;
    memset(signalfd_table, 0, sizeof(signalfd_table));
    signalfd_initialized = 1;
    kprintf("[OK] signalfd initialized\n");
}

/* Create a signalfd (returns fd or -1) */
int signalfd_create(int fd, const uint64_t *sigmask, size_t sigsetsize) {
    (void)fd;
    (void)sigsetsize;
    if (!signalfd_initialized) return -1;

    struct process *cur = process_get_current();
    if (!cur) return -1;

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (!signalfd_table[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENFILE;

    struct signalfd_entry *sfd = &signalfd_table[slot];
    sfd->in_use = 1;
    sfd->pid = cur->pid;
    sfd->sigmask = sigmask ? *sigmask : 0;
    sfd->queue_len = 0;
    wait_queue_init(&sfd->wq);

    /* Return a pseudo-fd (offset from base to avoid fd conflicts) */
    return 200 + slot;
}

/* Write a signal to signalfd queue */
int signalfd_push(int signo, struct siginfo *info) {
    if (!signalfd_initialized) return -1;

    struct process *cur = process_get_current();
    if (!cur) return -1;

    struct signalfd_siginfo ssi;
    memset(&ssi, 0, sizeof(ssi));
    ssi.ssi_signo = (uint32_t)signo;
    if (info) {
        ssi.ssi_code = info->si_code;
        ssi.ssi_pid = info->si_pid;
        ssi.ssi_uid = info->si_uid;
        ssi.ssi_addr = (uint64_t)info->si_addr;
        ssi.ssi_status = info->si_status;
    }

    /* Find matching signalfd for this process */
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (!signalfd_table[i].in_use) continue;
        if (signalfd_table[i].pid != cur->pid) continue;
        if (!(signalfd_table[i].sigmask & (1ULL << (signo - 1)))) continue;

        /* Append to queue */
        struct signalfd_entry *sfd = &signalfd_table[i];
        if (sfd->queue_len + (int)sizeof(ssi) <= (int)sizeof(sfd->queue)) {
            memcpy(sfd->queue + sfd->queue_len, &ssi, sizeof(ssi));
            sfd->queue_len += (int)sizeof(ssi);
        }
        wait_queue_wake(&sfd->wq);
        return 0;
    }
    return -1;
}

/* Read from signalfd */
int signalfd_read(int fd, void *buf, size_t count) {
    int slot = fd - 200;
    if (slot < 0 || slot >= SIGNALFD_MAX) return -EBADF;
    if (!signalfd_table[slot].in_use) return -EBADF;

    struct signalfd_entry *sfd = &signalfd_table[slot];

    /* Wait for data */
    while (sfd->queue_len == 0) {
        wait_queue_sleep(&sfd->wq);
    }

    size_t to_copy = (count < (size_t)sfd->queue_len) ? count : (size_t)sfd->queue_len;
    memcpy(buf, sfd->queue, to_copy);

    /* Shift remaining data */
    if ((int)to_copy < sfd->queue_len) {
        memmove(sfd->queue, sfd->queue + to_copy, (size_t)(sfd->queue_len - (int)to_copy));
    }
    sfd->queue_len -= (int)to_copy;

    return (int)to_copy;
}

/* Close signalfd */
int signalfd_close(int fd) {
    int slot = fd - 200;
    if (slot < 0 || slot >= SIGNALFD_MAX) return -EBADF;
    memset(&signalfd_table[slot], 0, sizeof(struct signalfd_entry));
    return 0;
}

/* ── signalfd_poll ────────────────────────────────────── */
int signalfd_poll(int fd)
{
    int slot = fd - 200;
    if (slot < 0 || slot >= SIGNALFD_MAX) return POLLNVAL;
    if (!signalfd_table[slot].in_use) return POLLNVAL;

    struct signalfd_entry *sfd = &signalfd_table[slot];
    int mask = POLLOUT; /* always writable */

    if (sfd->queue_len > 0)
        mask |= POLLIN;

    return mask;
}

/* ── signalfd_show_fdinfo ─────────────────────────────── */
int signalfd_show_fdinfo(int fd, char *buf, size_t size)
{
    if (!buf || size == 0) return -EINVAL;

    int slot = fd - 200;
    if (slot < 0 || slot >= SIGNALFD_MAX || !signalfd_table[slot].in_use) {
        int n = snprintf(buf, size, "signalfd:\t%d (invalid)\n", fd);
        return n < 0 ? -EINVAL : n;
    }

    struct signalfd_entry *sfd = &signalfd_table[slot];
    int n = snprintf(buf, size,
        "signalfd:\t%d\n"
        "pid:\t%u\n"
        "sigmask:\t0x%llx\n"
        "queue_len:\t%d\n",
        fd, sfd->pid,
        (unsigned long long)sfd->sigmask,
        sfd->queue_len);

    if (n < 0) return -EINVAL;
    if ((size_t)n >= size) return -ENOSPC;
    return n;
}

/* ── signalfd_release ─────────────────────────────────── */
int signalfd_release(int fd)
{
    int slot = fd - 200;
    if (slot < 0 || slot >= SIGNALFD_MAX) return -EBADF;
    memset(&signalfd_table[slot], 0, sizeof(struct signalfd_entry));
    return 0;
}
