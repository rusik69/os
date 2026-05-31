#ifndef WATCHDOG_H
#define WATCHDOG_H

#include "types.h"

/* Initialize the software watchdog with the given timeout in seconds.
 * The watchdog will reboot the system if watchdog_pet() is not called
 * within the timeout period. */
void watchdog_init(int timeout_seconds);

/* Pet (reset) the watchdog timer — call this periodically to prevent reboot. */
void watchdog_pet(void);

/* Stop/cancel the watchdog. */
void watchdog_stop(void);

#endif /* WATCHDOG_H */
