#ifndef SOFTIRQ_H
#define SOFTIRQ_H
#include "types.h"
#include "process.h"

typedef void (*softirq_handler)(void);
#define SOFTIRQ_NET_TX  0
#define SOFTIRQ_NET_RX  1
#define SOFTIRQ_TIMER   2
#define SOFTIRQ_TASKLET 3

/* Maximum number of softirqs to process in IRQ context before deferring
 * to ksoftirqd.  Prevents IRQ livelock when softirqs are raised faster
 * than they can be drained. */
#define SOFTIRQ_MAX_IRQ_PROCESS 4

void softirq_init(void);
int softirq_register(int nr, softirq_handler handler);
void softirq_raise(int nr);
void do_softirq(void);

/* ── ksoftirqd ────────────────────────────────────────────────────────────
 * Per-CPU kernel thread that processes softirqs deferred from IRQ context.
 * Created during kernel init; runs at SCHED_IDLE priority to avoid starving
 * user-space tasks. */
void create_ksoftirqd(void);

/* Signal ksoftirqd to check for pending work on the current CPU.
 * Called from do_softirq() when the per-CPU IRQ processing threshold
 * is exceeded. */
void softirq_wake_ksoftirqd(void);

#endif /* SOFTIRQ_H */
