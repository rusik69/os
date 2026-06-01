#ifndef LLIST_H
#define LLIST_H

#include "types.h"

/*
 * Lock-less singly-linked list.
 *
 * Based on the Linux llist concept.  Synchronisation is done via
 * cmpxchg on the head pointer; no external locks are needed.
 *
 * struct llist_head  - points to the first node (or NULL).
 * struct llist_node  - embedded in user data, contains a "next" pointer.
 *
 * The "next" field of the last node must be NULL.
 */

struct llist_node {
    struct llist_node *next;
};

struct llist_head {
    struct llist_node *first;
};

#define LLIST_HEAD_INIT(name)  { NULL }
#define LLIST_HEAD(name)       struct llist_head name = LLIST_HEAD_INIT(name)

static inline void init_llist_head(struct llist_head *list)
{
    list->first = NULL;
}

/*
 * llist_add  - Atomically add a new node at the head.
 */
void llist_add(struct llist_node *new, struct llist_head *head);

/*
 * llist_add_batch  - Atomically add a chain of nodes (first through last).
 * first->...->last are already linked internally.
 */
void llist_add_batch(struct llist_node *first, struct llist_node *last,
                     struct llist_head *head);

/*
 * llist_del_all  - Atomically remove and return the entire list.
 * Returns pointer to the first node (or NULL if empty).
 */
struct llist_node *llist_del_all(struct llist_head *head);

/*
 * llist_del_first  - Atomically remove and return the first node.
 * Returns the removed node, or NULL if the list was empty.
 */
struct llist_node *llist_del_first(struct llist_head *head);

/*
 * llist_empty  - Returns non-zero (true) if the list is empty.
 */
static inline int llist_empty(const struct llist_head *head)
{
    return head->first == NULL;
}

/*
 * llist_entry  - Convert an llist_node back to the containing struct.
 */
#define llist_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))

/*
 * llist_for_each  - Iterate over nodes in an llist.
 * pos must be a 'struct llist_node *'.
 */
#define llist_for_each(pos, head) \
    for (pos = (head)->first; pos; pos = pos->next)

void llist_init(void);

#endif /* LLIST_H */
