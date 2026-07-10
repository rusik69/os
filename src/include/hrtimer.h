#ifndef HRTIMER_H
#define HRTIMER_H
#include "types.h"
#include "timer.h"
typedef uint64_t ktime_t;
#define KTIME_MAX ((uint64_t)-1)
struct hrtimer {
    uint64_t expires;
    void (*function)(void *);
    void *data;
    int state;
    int timer_id;   /* underlying dynamic timer slot, -1 if not scheduled */
};
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
void hrtimer_init(struct hrtimer *timer, void (*function)(void *), void *data);
int hrtimer_start(struct hrtimer *timer, uint64_t ns);
int hrtimer_cancel(struct hrtimer *timer);
uint64_t hrtimer_get_remaining(struct hrtimer *timer);
int hrtimer_active(struct hrtimer *timer);
static inline ktime_t ns_to_ktime(uint64_t ns) { return ns; }
static inline uint64_t ktime_to_ns(ktime_t kt) { return kt; }
#endif
