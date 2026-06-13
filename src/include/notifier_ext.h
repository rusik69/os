#ifndef NOTIFIER_EXT_H
#define NOTIFIER_EXT_H

#include "types.h"

/*
 * Notifier chain extensions.
 *
 * Provides a generic notifier chain mechanism allowing kernel subsystems
 * to register callbacks that are invoked when events occur.  Supports
 * priority-ordered chains with return-code aggregation.
 *
 * Based on the Linux notifier chain concept.
 */

#define NOTIFIER_PRIO_MAX       0x7FFFFFFF

/* Return codes from notifier callbacks */
#define NOTIFY_DONE             0x00    /* no action taken */
#define NOTIFY_OK               0x01    /* handled */
#define NOTIFY_BAD              0x02    /* a problem occurred */
#define NOTIFY_STOP_MASK        0x80    /* stop processing further callbacks */

/* Priorities for common subsystems */
#define NOTIFIER_PRIO_DEFAULT   0
#define NOTIFIER_PRIO_HIGH      100
#define NOTIFIER_PRIO_LOW      -100

struct notifier_block;

typedef int (*notifier_fn_t)(struct notifier_block *nb,
                             unsigned long action, void *data);

struct notifier_block {
    notifier_fn_t       notify;     /* callback function */
    struct notifier_block *next;    /* linked-list chain */
    int                 priority;   /* higher = called first */
};

struct notifier_head {
    struct notifier_block *head;    /* head of the chain */
};

/*
 * NOTIFIER_HEAD_INIT  - Static initialiser for a notifier head.
 */
#define NOTIFIER_HEAD_INIT(name)  { .head = NULL }

/*
 * notifier_chain_register  - Insert @nb into chain @nh in priority order.
 * Returns 0 on success, -1 if @nb is already registered.
 */
int notifier_ext_chain_register(struct notifier_head *nh,
                            struct notifier_block *nb);

/*
 * notifier_chain_unregister  - Remove @nb from chain @nh.
 * Returns 0 on success, -1 if not found.
 */
int notifier_ext_chain_unregister(struct notifier_head *nh,
                              struct notifier_block *nb);

/*
 * notifier_call_chain  - Walk the chain and invoke each callback.
 * Stops if a callback returns NOTIFY_BAD | NOTIFY_STOP_MASK.
 * Returns the last return code (NOTIFY_DONE if chain is empty).
 */
int notifier_ext_call_chain(struct notifier_head *nh,
                        unsigned long action, void *data);

/*
 * notifier_ext_init  - Initialise the notifier subsystem.
 */
void notifier_ext_init(void);

#endif /* NOTIFIER_EXT_H */
