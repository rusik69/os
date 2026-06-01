#ifndef NOTIFIER_H
#define NOTIFIER_H
#include "types.h"
/* Notifier chain types */
#define NOTIFIER_PANIC      0
#define NOTIFIER_DIE        1
#define NOTIFIER_REBOOT     2
#define NOTIFIER_NET_EVENT  3
#define NOTIFIER_CPU_HP     4
#define NOTIFIER_MAX        8
struct notifier_block {
    int (*notifier_call)(struct notifier_block *, unsigned long, void *);
    struct notifier_block *next;
};
void notifier_init(void);
int notifier_chain_register(int type, struct notifier_block *nb);
int notifier_chain_unregister(int type, struct notifier_block *nb);
int notifier_call_chain(int type, unsigned long v, void *data);
#endif
