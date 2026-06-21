#include "klist.h"
#include "printf.h"
#include "heap.h"
#include "kernel.h"

void klist_init(struct klist *list,
                klist_get_func get, klist_put_func put)
{
    INIT_LIST_HEAD(&list->head);
    spinlock_init(&list->lock);
    list->get = get;
    list->put = put;
}

void klist_node_init(struct klist *list, struct klist_node *n)
{
    spinlock_init(&n->lock);
    n->refcount = 1;      /* reference held by the list itself */
    n->list     = list;
    INIT_LIST_HEAD(&n->node);
}

void klist_add_head(struct klist_node *n)
{
    struct klist *list = n->list;

    if (!list)
        return;

    spinlock_acquire(&list->lock);
    list_add(&n->node, &list->head);
    spinlock_release(&list->lock);
}

void klist_add_tail(struct klist_node *n)
{
    struct klist *list = n->list;

    if (!list)
        return;

    spinlock_acquire(&list->lock);
    list_add_tail(&n->node, &list->head);
    spinlock_release(&list->lock);
}

void klist_remove(struct klist_node *n)
{
    struct klist *list = n->list;

    if (!list)
        return;

    spinlock_acquire(&list->lock);
    list_del(&n->node);
    spinlock_release(&list->lock);

    /* Drop the list's reference */
    if (list->put)
        list->put(n);
}

int klist_empty(struct klist *list)
{
    int empty;
    spinlock_acquire(&list->lock);
    empty = list_empty(&list->head);
    spinlock_release(&list->lock);
    return empty;
}

int klist_put(struct klist_node *n)
{
    int old;
    spinlock_acquire(&n->lock);
    old = n->refcount--;
    spinlock_release(&n->lock);
    return old;
}

struct klist_node *klist_get(struct klist_node *n)
{
    if (!n)
        return NULL;
    spinlock_acquire(&n->lock);
    n->refcount++;
    spinlock_release(&n->lock);
    return n;
}

struct klist_node *klist_next(struct klist_node *n)
{
    struct klist *list;
    struct list_head *next;

    if (!n)
        return NULL;  /* iterator not properly started */

    list = n->list;
    if (!list)
        return NULL;

    spinlock_acquire(&list->lock);
    next = n->node.next;
    if (next == &list->head)
        next = NULL;
    spinlock_release(&list->lock);

    if (!next)
        return NULL;

    return list_entry(next, struct klist_node, node);
}

void klist_init_global(void)
{
    kprintf("[OK] klist: Reference-counted linked lists initialised\n");
}

/* ── Stub: klist_del ─────────────────────────────── */
int klist_del(void *n)
{
    (void)n;
    kprintf("[klist] klist_del: not yet implemented\n");
    return 0;
}
/* ── Stub: klist_iter_init ─────────────────────────────── */
int klist_iter_init(void *k, void *i)
{
    (void)k;
    (void)i;
    kprintf("[klist] klist_iter_init: not yet implemented\n");
    return 0;
}
