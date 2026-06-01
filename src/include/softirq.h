#ifndef SOFTIRQ_H
#define SOFTIRQ_H
#include "types.h"
typedef void (*softirq_handler)(void);
#define SOFTIRQ_NET_TX  0
#define SOFTIRQ_NET_RX  1
#define SOFTIRQ_TIMER   2
#define SOFTIRQ_TASKLET 3
void softirq_init(void);
int softirq_register(int nr, softirq_handler handler);
void softirq_raise(int nr);
void do_softirq(void);
#endif
