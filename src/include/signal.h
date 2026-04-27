#ifndef SIGNAL_H
#define SIGNAL_H

#include "types.h"

/* Signal numbers */
#define SIGKILL  9
#define SIGTERM  15
#define SIGSTOP  19
#define SIGCONT  18
#define SIGUSR1  10
#define SIGUSR2  12

#define SIG_MAX  32
#define SIG_DFL  ((signal_handler_t)0)  /* Default action */
#define SIG_IGN  ((signal_handler_t)1)  /* Ignore */

typedef void (*signal_handler_t)(int signum);

/* Send signal to process by pid; 0 = success, -1 = not found */
int signal_send(uint32_t pid, int signum);

/* Check and deliver any pending signals for the current process.
 * Called by scheduler before returning to a READY process. */
void signal_check(void);

/* Register a signal handler for the current process */
void signal_register(int signum, signal_handler_t handler);

#endif
