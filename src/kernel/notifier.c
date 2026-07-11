#include "notifier.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
/* Local max notifiers may differ from the public NOTIFIER_MAX constant */
#define NOTIFIER_LIST_SIZE 32
static struct notifier_block *notifier_list[NOTIFIER_MAX];
static int notifier_init_done = 0;
static spinlock_t notifier_lock = SPINLOCK_INIT;
void notifier_init(void) {
    memset(notifier_list, 0, sizeof(notifier_list));
    notifier_init_done = 1;
    kprintf("[OK] Notifier chains initialized\n");
}
int notifier_chain_register(int type, struct notifier_block *nb) {
    if (!notifier_init_done || type < 0 || type >= NOTIFIER_MAX || !nb) return -1;
    uint64_t flags;
    spinlock_irqsave_acquire(&notifier_lock, &flags);
    nb->next = notifier_list[type];
    notifier_list[type] = nb;
    spinlock_irqsave_release(&notifier_lock, flags);
    return 0;
}
int notifier_chain_unregister(int type, struct notifier_block *nb) {
    if (!notifier_init_done || type < 0 || type >= NOTIFIER_MAX || !nb) return -1;
    uint64_t flags;
    spinlock_irqsave_acquire(&notifier_lock, &flags);
    struct notifier_block **pp = &notifier_list[type];
    while (*pp) {
        if (*pp == nb) { *pp = nb->next; spinlock_irqsave_release(&notifier_lock, flags); return 0; }
        pp = &(*pp)->next;
    }
    spinlock_irqsave_release(&notifier_lock, flags);
    return -1;
}
int notifier_call_chain(int type, unsigned long v, void *data) {
    if (!notifier_init_done || type < 0 || type >= NOTIFIER_MAX) return 0;
    int ret = 0;
    /* Snapshot the chain under lock to prevent concurrent modification
     * during traversal. Callbacks are invoked outside the lock so they
     * may safely register/unregister on the same chain type. */
    struct notifier_block *chain[NOTIFIER_LIST_SIZE];
    int count = 0;
    uint64_t flags;
    spinlock_irqsave_acquire(&notifier_lock, &flags);
    struct notifier_block *nb = notifier_list[type];
    while (nb && count < NOTIFIER_LIST_SIZE) {
        chain[count++] = nb;
        nb = nb->next;
    }
    spinlock_irqsave_release(&notifier_lock, flags);
    for (int i = 0; i < count; i++) {
        if (chain[i]->notifier_call)
            ret = chain[i]->notifier_call(chain[i], v, data);
    }
    return ret;
}

/* ── notifier_register_priority: Register notifier with priority ───── */
static int notifier_register_priority(void *nl, void *n, int priority)
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
static int notifier_set_priority(void *n, int priority)
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
