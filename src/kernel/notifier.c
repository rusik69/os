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

/* ── notifier_register_priority: Register notifier with priority ───── */
int notifier_register_priority(void *nl, void *n, int priority)
{
    (void)priority;
    if (!nl || !n) return -EINVAL;

    /* The current implementation ignores priority and just appends.
     * This wrapper converts the generic args to our typed API. */
    struct notifier_block *nb = (struct notifier_block *)n;
    int type = (int)(uintptr_t)nl; /* the type is passed via nl */

    kprintf("[notifier] notifier_register_priority: type=%d, nb=0x%llx, priority=%d\n",
            type, (unsigned long long)(uintptr_t)nb, priority);

    return notifier_chain_register(type, nb);
}
/* ── notifier_set_priority: Update notifier priority ────────────────── */
int notifier_set_priority(void *n, int priority)
{
    (void)n;
    (void)priority;
    /* This is a no-op in the current simple implementation since
     * priorities are not stored. In a full implementation this would
     * re-sort the chain. */
    kprintf("[notifier] notifier_set_priority: nb=0x%llx, priority=%d\n",
            (unsigned long long)(uintptr_t)n, priority);
    return 0;
}
