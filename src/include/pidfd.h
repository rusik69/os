#ifndef PIDFD_H
#define PIDFD_H

#include "types.h"
#include "signal.h"   /* struct siginfo */

/*
 * pidfd — file-descriptor based process tracking (Linux-style).
 * A pidfd provides a race-free way to refer to a process and
 * send signals to it.
 */

#define PIDFD_MAX  32

int pidfd_open(uint32_t pid, uint32_t flags);
int pidfd_send_signal(int pidfd, int sig, struct siginfo *info, uint32_t flags);

/* Init called during kernel boot */
void pidfd_init(void);

#endif /* PIDFD_H */
