#ifndef SIGNALFD_H
#define SIGNALFD_H

#include "types.h"

/*
 * signalfd.h — Signal file descriptor (siginfo extraction)
 *
 * Provides a file descriptor interface for reading pending signal info.
 * When a signal matching the signalfd's mask is delivered, its siginfo_t
 * data is queued and can be read as struct signalfd_siginfo entries.
 *
 * Format follows Linux struct signalfd_siginfo for compatibility.
 */

/* signalfd_create flags */
#define SFD_CLOEXEC 02000000
#define SFD_NONBLOCK 04000

/* signalfd_siginfo structure — 128 bytes (Linux compatible subset) */
struct signalfd_siginfo {
    uint32_t ssi_signo;     /* Signal number */
    int32_t  ssi_errno;     /* Error number (unused) */
    int32_t  ssi_code;      /* Signal code (SI_USER, SI_QUEUE, etc.) */
    uint32_t ssi_pid;       /* Sender's PID */
    uint32_t ssi_uid;       /* Sender's UID */
    int32_t  ssi_fd;        /* File descriptor (SIGIO) */
    uint32_t ssi_tid;       /* Kernel timer ID (POSIX timers) */
    uint32_t ssi_band;      /* Band event (SIGIO) */
    uint32_t ssi_overrun;   /* Overrun count (POSIX timers) */
    uint32_t ssi_trapno;    /* Trap number (x86) */
    int32_t  ssi_status;    /* Exit status (SIGCHLD) */
    int32_t  ssi_int;       /* Integer sent by sigqueue */
    uint64_t ssi_ptr;       /* Pointer sent by sigqueue */
    uint64_t ssi_utime;     /* User time (SIGCHLD) */
    uint64_t ssi_stime;     /* System time (SIGCHLD) */
    uint64_t ssi_addr;      /* Fault address (SIGSEGV, SIGBUS) */
    uint16_t ssi_addr_lsb;  /* LSB of fault address (SIGBUS) */
    uint8_t  __pad[46];     /* Pad to 128 bytes */
} __attribute__((packed));

/* Ensure the struct is exactly 128 bytes */
_Static_assert(sizeof(struct signalfd_siginfo) == 128,
               "signalfd_siginfo must be 128 bytes");

/* Initialise the signalfd subsystem */
void signalfd_init(void);

/* Called from signal delivery — enqueue siginfo for matching signalfds */
void signalfd_notify_ext(int signum, int si_code,
                         uint32_t si_pid, uint32_t si_uid,
                         uint64_t si_addr, int si_status);

int signalfd_create(uint64_t mask);

/* Read one signalfd_siginfo entry from the slot. Returns bytes read or -1. */
int signalfd_read_info(int slot, struct signalfd_siginfo *out);

/* Poll support: returns a bitmask of POLLIN/POLLOUT */
int signalfd_poll(int fd, void *pt);

#endif /* SIGNALFD_H */
