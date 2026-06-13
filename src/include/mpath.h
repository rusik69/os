#ifndef MPATH_H
#define MPATH_H

#include "types.h"

/* ── Multipath I/O ───────────────────────────────────────────────── */

/* Initialize the multipath subsystem */
void mpath_init(void);

/* Create a multipath device */
int mpath_create(const char *name, int selector);

/* Destroy a multipath device */
int mpath_destroy(int mpath_id);

/* Add a path to a multipath device */
int mpath_add_path(int mpath_id, int dev_id);

/* Remove a path from a multipath device */
int mpath_remove_path(int mpath_id, int dev_id);

/* Select a path for the next I/O */
int mpath_select_path(int mpath_id);

/* Complete an I/O on a path (call after I/O finishes) */
void mpath_complete_io(int mpath_id, int dev_id, int success,
                       uint64_t start_tick);

/* Set the path selector algorithm */
int mpath_set_selector(int mpath_id, int selector);

/* Fail a path */
int mpath_fail_path(int mpath_id, int dev_id);

/* Restore a path */
int mpath_restore_path(int mpath_id, int dev_id);

/* Query device status */
int mpath_status(int mpath_id, char *buf, int max);

/* Selector constants */
#define MPATH_SEL_ROUND_ROBIN   0
#define MPATH_SEL_LEAST_QUEUED  1
#define MPATH_SEL_SERVICE_TIME  2

#endif /* MPATH_H */
