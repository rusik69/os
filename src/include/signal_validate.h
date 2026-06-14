#ifndef SIGNAL_VALIDATE_H
#define SIGNAL_VALIDATE_H

#include "signal.h"

/*
 * signal_validate.c — siginfo validation before delivery to userspace
 */

/* Validate siginfo fields before delivery.
 * @is_from_userspace: 1 if originating from userspace (tgkill/sigqueue), 0 if kernel.
 * Returns 0 if valid, negative errno if suspicious (caller should still deliver sanitized). */
int signal_validate_siginfo(struct siginfo *info, int is_from_userspace);

/* Hook called at signal delivery point (just before userspace handler sees it). */
int signal_validate_on_delivery(struct siginfo *info);

/* Set debug flag for preserving kernel addresses in si_addr */
void signal_validate_set_debug(int val);

#endif /* SIGNAL_VALIDATE_H */
