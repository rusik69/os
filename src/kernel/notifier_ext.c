#include "notifier_ext.h"
#include "printf.h"
#include "kernel.h"
#include "spinlock.h"

/*
 * Notifier chain: priority-ordered list of callbacks.
 *
 * Registration inserts in priority order (highest first).
 * Invocation walks the chain until a callback returns
 * (NOTIFY_BAD | NOTIFY_STOP_MASK).
 */

static spinlock_t notifier_lock;

int notifier_ext_chain_register(struct notifier_head *nh,
                            struct notifier_block *nb)
{
    struct notifier_block **pp;

    if (!nh || !nb || !nb->notify)
        return -1;

    spinlock_acquire(&notifier_lock);

    /* Check for duplicates */
    for (struct notifier_block *p = nh->head; p; p = p->next) {
        if (p == nb) {
            spinlock_release(&notifier_lock);
            return -1;
        }
    }

    /* Insert in priority order (highest first) */
    pp = &nh->head;
    while (*pp && (*pp)->priority >= nb->priority)
        pp = &(*pp)->next;

    nb->next = *pp;
    *pp = nb;

    spinlock_release(&notifier_lock);
    return 0;
}

int notifier_ext_chain_unregister(struct notifier_head *nh,
                              struct notifier_block *nb)
{
    struct notifier_block **pp;

    if (!nh || !nb)
        return -1;

    spinlock_acquire(&notifier_lock);

    pp = &nh->head;
    while (*pp) {
        if (*pp == nb) {
            *pp = nb->next;
            nb->next = NULL;
            spinlock_release(&notifier_lock);
            return 0;
        }
        pp = &(*pp)->next;
    }

    spinlock_release(&notifier_lock);
    return -1;
}

int notifier_ext_call_chain(struct notifier_head *nh,
                        unsigned long action, void *data)
{
    struct notifier_block *nb;
    int ret = NOTIFY_DONE;

    if (!nh)
        return NOTIFY_DONE;

    spinlock_acquire(&notifier_lock);
    nb = nh->head;
    while (nb) {
        ret = nb->notify(nb, action, data);
        if (ret & NOTIFY_STOP_MASK)
            break;
        nb = nb->next;
    }
    spinlock_release(&notifier_lock);

    return ret;
}

void notifier_ext_init(void)
{
    spinlock_init(&notifier_lock);
    kprintf("[OK] notifier_ext: Notifier chain extensions initialised\n");
}

/* ── Stub: notifier_ext_register ─────────────────────────────── */
int notifier_ext_register(void *n)
{
    (void)n;
    kprintf("[notifier] notifier_ext_register: not yet implemented\n");
    return 0;
}
/* ── Stub: notifier_ext_unregister ─────────────────────────────── */
int notifier_ext_unregister(void *n)
{
    (void)n;
    kprintf("[notifier] notifier_ext_unregister: not yet implemented\n");
    return 0;
}
/* ── Stub: notifier_ext_call ─────────────────────────────── */
int notifier_ext_call(void *n, unsigned long val, void *v)
{
    (void)n;
    (void)val;
    (void)v;
    kprintf("[notifier] notifier_ext_call: not yet implemented\n");
    return 0;
}
