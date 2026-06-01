#include "softirq.h"
#include "printf.h"
#include "string.h"
#define SOFTIRQ_MAX 16
static softirq_handler softirq_handlers[SOFTIRQ_MAX];
static uint32_t softirq_pending = 0;
void softirq_init(void) {
    memset(softirq_handlers, 0, sizeof(softirq_handlers));
    softirq_pending = 0;
    kprintf("[OK] SoftIRQ subsystem initialized\n");
}
int softirq_register(int nr, softirq_handler handler) {
    if (nr < 0 || nr >= SOFTIRQ_MAX || !handler) return -1;
    softirq_handlers[nr] = handler;
    return 0;
}
void softirq_raise(int nr) {
    if (nr >= 0 && nr < SOFTIRQ_MAX)
        __atomic_fetch_or(&softirq_pending, 1U << nr, __ATOMIC_SEQ_CST);
}
void do_softirq(void) {
    uint32_t pending = __atomic_exchange_n(&softirq_pending, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; pending && i < SOFTIRQ_MAX; i++) {
        if (pending & (1U << i)) {
            pending &= ~(1U << i);
            if (softirq_handlers[i])
                softirq_handlers[i]();
        }
    }
}
