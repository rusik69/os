#include "ratelimit.h"
#include "printf.h"
#include "timer.h"
int __ratelimit(struct ratelimit_state *rs) {
    uint64_t now = timer_get_ticks();
    if (!rs->interval) { rs->interval = 5; rs->burst = 10; }
    if ((uint64_t)(now - rs->begin) >= (uint64_t)(rs->interval * 10)) {
        rs->begin = now;
        rs->printed = 0;
    }
    if (rs->printed < rs->burst) {
        rs->printed++;
        return 1;
    }
    return 0;
}
