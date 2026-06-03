#include "notifier.h"
#include "printf.h"
#include "string.h"
/* Local max notifiers may differ from the public NOTIFIER_MAX constant */
#define NOTIFIER_LIST_SIZE 32
static struct notifier_block *notifier_list[NOTIFIER_LIST_SIZE];
static int notifier_init_done = 0;
void notifier_init(void) {
    memset(notifier_list, 0, sizeof(notifier_list));
    notifier_init_done = 1;
    kprintf("[OK] Notifier chains initialized\n");
}
int notifier_chain_register(int type, struct notifier_block *nb) {
    if (!notifier_init_done || type < 0 || type >= NOTIFIER_MAX || !nb) return -1;
    nb->next = notifier_list[type];
    notifier_list[type] = nb;
    return 0;
}
int notifier_chain_unregister(int type, struct notifier_block *nb) {
    if (!notifier_init_done || type < 0 || type >= NOTIFIER_MAX || !nb) return -1;
    struct notifier_block **pp = &notifier_list[type];
    while (*pp) {
        if (*pp == nb) { *pp = nb->next; return 0; }
        pp = &(*pp)->next;
    }
    return -1;
}
int notifier_call_chain(int type, unsigned long v, void *data) {
    if (!notifier_init_done || type < 0 || type >= NOTIFIER_MAX) return 0;
    int ret = 0;
    struct notifier_block *nb = notifier_list[type];
    while (nb) {
        if (nb->notifier_call)
            ret = nb->notifier_call(nb, v, data);
        nb = nb->next;
    }
    return ret;
}
