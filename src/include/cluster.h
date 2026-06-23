#ifndef CLUSTER_H
#define CLUSTER_H

/* Shared cluster interface.
 *
 * NODE_MAX, NODE_ID_MAX, NODE_HOSTNAME_MAX are defined per translation unit.
 * Reconcile to a single shared constant once cluster.c and node.c unify.
 */

/* Get the local node index (or -1 if not registered yet).
 * Implemented in node.c where the node registration logic lives. */
int cluster_get_local_node_idx(void);

#endif /* CLUSTER_H */