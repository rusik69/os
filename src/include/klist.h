#ifndef KLIST_H
#define KLIST_H

#include "types.h"
#include "list.h"
#include "spinlock.h"

struct klist;

struct klist_node {
    struct list_head  node;
    spinlock_t        lock;
    int               refcount;
    struct klist     *list;   /* back-pointer to owning list */
};

struct klist {
    struct list_head  head;
    spinlock_t        lock;
    void              (*get)(struct klist_node *);
    void              (*put)(struct klist_node *);
} __cacheline_aligned;

/* Life-cycle callbacks for reference-counted nodes. */
typedef void (*klist_get_func)(struct klist_node *n);
typedef void (*klist_put_func)(struct klist_node *n);

/* Initialise an empty klist with optional get/put callbacks. */
void klist_init(struct klist *list,
                klist_get_func get, klist_put_func put);

/* Initialise a klist node (call before adding to a list). */
void klist_node_init(struct klist *list, struct klist_node *n);

/* Add a node to the front/back of the list, taking a reference. */
void klist_add_head(struct klist_node *n);
void klist_add_tail(struct klist_node *n);

/* Remove a node from its list, dropping the reference. */
void klist_remove(struct klist_node *n);

/* Return true if the list is empty. */
int klist_empty(struct klist *list);

/* Atomically test-and-dec refcount. Returns old refcount before decrement. */
int klist_put(struct klist_node *n);

/* Atomically inc refcount. */
struct klist_node *klist_get(struct klist_node *n);

/* Iterate over all nodes (caller must hold list lock). */
struct klist_node *klist_next(struct klist_node *n);

#define klist_for_each(pos, list) \
    for (pos = klist_next(NULL); pos; pos = klist_next(pos))

void klist_init_global(void);

#endif /* KLIST_H */
