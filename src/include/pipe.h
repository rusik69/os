#ifndef PIPE_H
#define PIPE_H

#include "types.h"
#include "waitqueue.h"

#define PIPE_BUF_SIZE 4096
#define PIPE_MAX      16

struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    int      read_pos;
    int      write_pos;
    int      count;
    int      readers;   /* number of open read ends */
    int      writers;   /* number of open write ends */
    int      in_use;
    uint8_t  flags;     /* PIPE_FLAG_NONBLOCK etc. */
    uint32_t sigio_pid; /* PID to receive SIGIO (0 = none) */
    struct wait_queue read_wq;   /* waiters waiting for data */
    struct wait_queue write_wq;  /* waiters waiting for space */
};

#define PIPE_FLAG_NONBLOCK 1

/* Allocate a new pipe; returns its index or -1 on error */
int pipe_create(void);

/* Write len bytes into pipe. Blocks (yields) if full. Returns bytes written or -1. */
int pipe_write(int pipe_id, const void *buf, int len);

/* Read up to len bytes from pipe. Blocks if empty. Returns bytes read or 0 on EOF. */
int pipe_read(int pipe_id, void *buf, int len);

/* Close the read or write end of a pipe */
void pipe_close_read(int pipe_id);
void pipe_close_write(int pipe_id);

/* Return current byte count in pipe */
int pipe_available(int pipe_id);

void pipe_init(void);
void pipe_set_nonblock(int pipe_id, int nonblock);
void pipe_set_sigio(int pipe_id, uint32_t pid);

/* Poll a pipe for readiness.
 * @pipe_id     Pipe index
 * @is_read_end 1 for read end, 0 for write end
 * @return      Bitmask of POLLIN|POLLOUT|POLLHUP|POLLERR|POLLNVAL
 */
int pipe_poll(int pipe_id, int is_read_end);

#endif
