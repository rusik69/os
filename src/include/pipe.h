#ifndef PIPE_H
#define PIPE_H

#include "types.h"
#include "waitqueue.h"

#define PIPE_BUF_SIZE      4096    /* POSIX minimum buffer size */
#define PIPE_DEFAULT_SIZE   65536  /* default pipe capacity (64KB, Linux default) */
#define PIPE_MAX_SIZE      1048576 /* maximum pipe capacity (1MB) */
#define PIPE_MAX            16

#define PIPE_FLAG_NONBLOCK  1
#define PIPE_FLAG_PACKET    2

struct pipe {
    uint8_t  *buf;          /* heap-allocated buffer */
    int      capacity;      /* current buffer capacity */
    int      read_pos;
    int      write_pos;
    int      count;
    int      readers;       /* number of open read ends */
    int      writers;       /* number of open write ends */
    int      in_use;
    uint8_t  flags;         /* PIPE_FLAG_NONBLOCK etc. */
    uint32_t sigio_pid;     /* PID to receive SIGIO (0 = none) */
    struct wait_queue read_wq;   /* waiters waiting for data */
    struct wait_queue write_wq;  /* waiters waiting for space */
};

/* Allocate a new pipe; returns its index or -1 on error */
int pipe_create(void);

/* Write len bytes into pipe. Blocks (yields) if full. Returns bytes written or -1. */
int pipe_write(int pipe_id, const void *buf, int len);

/* Read at most len bytes from pipe. Blocks (yields) if empty. Returns bytes read or -1. */
int pipe_read(int pipe_id, void *buf, int len);

/* Close one end of a pipe. Returns 0 on success. */
int pipe_close(int pipe_id, int is_write_end);

/* Resize pipe buffer to new_capacity. Must be <= PIPE_MAX_SIZE.
 * Returns the new capacity, or -1 on error. */
int pipe_set_capacity(int pipe_id, int new_capacity);

/* Get current pipe buffer capacity. */
int pipe_get_capacity(int pipe_id);

/* Initialize pipe subsystem */
void pipe_init(void);

#endif /* PIPE_H */
