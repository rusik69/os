#ifndef TIMERFD_H
#define TIMERFD_H
#include "types.h"

/* timerfd flags */
#define TFD_CLOEXEC      02000000
#define TFD_NONBLOCK     00004000
#define TFD_TIMER_ABSTIME (1 << 0)

/* clock IDs for timerfd */
#define CLOCK_REALTIME          0
#define CLOCK_MONOTONIC         1
#define CLOCK_BOOTTIME          7

struct itimerspec {
    struct timespec it_interval;  /* timer period */
    struct timespec it_value;     /* timer expiration */
};

void timerfd_init(void);
int timerfd_create(int clockid);
int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);
int timerfd_read(int fd, uint64_t *val);
void timerfd_close(int fd);

#endif
