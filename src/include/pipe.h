#ifndef PIPE_H
#define PIPE_H

#include "types.h"

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
};

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

#endif
