#include "llist.h"
#include "printf.h"
#include "kernel.h"

/*
 * Lock-less singly-linked list using cmpxchg.
 *
 * All modifications to the head pointer are done via atomic compare-and-swap.
 * No MMU-based locking is required for single-word head updates.
 */

#define __LLIST_CMPXCHG(ptr, old, new)                              \
    __sync_val_compare_and_swap(ptr, old, new)

void llist_add(struct llist_node *new, struct llist_head *head)
{
    struct llist_node *first;

    do {
        first = head->first;
        new->next = first;
    } while (__LLIST_CMPXCHG(&head->first, first, new) != first);
}

void llist_add_batch(struct llist_node *first, struct llist_node *last,
                     struct llist_head *head)
{
    struct llist_node *old_first;

    do {
        old_first = head->first;
        last->next = old_first;
    } while (__LLIST_CMPXCHG(&head->first, old_first, first) != old_first);
}

struct llist_node *llist_del_all(struct llist_head *head)
{
    struct llist_node *first;

    do {
        first = head->first;
    } while (__LLIST_CMPXCHG(&head->first, first, NULL) != first);

    return first;
}

struct llist_node *llist_del_first(struct llist_head *head)
{
    struct llist_node *first, *next;

    do {
        first = head->first;
        if (!first)
            return NULL;
        next = first->next;
    } while (__LLIST_CMPXCHG(&head->first, first, next) != first);

    return first;
}

void llist_init(void)
{
    kprintf("[OK] llist: Lock-less singly-linked list initialised\n");
}
