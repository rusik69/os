#ifndef FIFO_H
#define FIFO_H

#include "types.h"
#include "pipe.h"

/* Named pipes (FIFOs) — registered as S_IFIFO in VFS.
 *
 * Implementation uses the existing pipe infrastructure:
 * fifo_create() creates a FIFO inode, fifo_open() associates
 * it with a pipe descriptor.
 *
 * FIFOs are stored in a simple table, max 16 entries.
 */

#define FIFO_MAX 16

struct fifo {
    char     path[64];
    int      pipe_id;    /* index into pipe_table */
    uint32_t inode;      /* fake inode number */
    int      in_use;
};

/* Create a named pipe (FIFO) at the given VFS path */
int fifo_create(const char *path);

/* Open a FIFO: returns the pipe_id, or -1 on error */
int fifo_open(const char *path);

/* Check if a path refers to a FIFO */
int fifo_is_fifo(const char *path);

/* Remove a FIFO */
int fifo_unlink(const char *path);

/* Initialise FIFO subsystem */
void fifo_init(void);

#endif /* FIFO_H */
