#ifndef AIO_ENHANCED_H
#define AIO_ENHANCED_H

#include "types.h"

/* Maximum number of AIO contexts */
#define AIO_EXT_MAX_CTX 4

/* Completion ring size per context */
#define AIO_EXT_RING_SIZE 64

/* I/O control block — describes an asynchronous I/O operation */
struct iocb {
    uint64_t aio_fd;       /* file descriptor to operate on */
    uint64_t aio_offset;   /* file offset */
    uint64_t aio_buf;      /* pointer to data buffer (user-space address) */
    uint64_t aio_nbytes;   /* number of bytes to transfer */
    int      aio_rw_flags; /* RWF_* flags (e.g. RWF_APPEND, RWF_DSYNC) */
    uint16_t aio_lio_opcode; /* IOCB_CMD_PREAD=0, IOCB_CMD_PWRITE=1,
                                IOCB_CMD_POLL=2 */
    int16_t  aio_reqprio;   /* request priority */
    /* Internal tracking */
    uint64_t aio_data;       /* user data cookie */
};

/* Completion event — returned by aio_getevents */
struct io_event {
    uint64_t data;        /* user data from iocb.aio_data */
    uint64_t obj;         /* pointer to the iocb that completed */
    uint64_t res;         /* result code (bytes transferred or error) */
    uint64_t res2;        /* secondary result (unused) */
};

/* AIO context extended descriptor */
struct aio_context_ext {
    uint64_t ctx_id;
    int      nr_events;
};

/* Submit asynchronous poll on a file descriptor.
 * fd: file descriptor. events: POLLIN/POLLOUT/etc.
 * Returns 0 on success, negative on error. */
int aio_ext_poll(int fd, uint64_t events);

/* Reap completed I/O events from a context.
 * ctx_id: context identifier. min_nr: minimum events to return.
 * max_nr: maximum events to return.
 * events: array to fill (must be at least max_nr elements).
 * timeout: NULL to block, {0,0} for non-block.
 * Returns number of events reaped, or negative on error. */
int aio_ext_getevents(uint64_t ctx_id, long min_nr, long max_nr,
                      struct io_event *events, struct timespec *timeout);

/* Set up an AIO context.
 * nr_events: max events the context can hold.
 * ctx_out: filled with the new context identifier. */
int aio_ext_setup(int nr_events, struct aio_context_ext *ctx_out);

/* Destroy an AIO context.
 * ctx_id: context identifier to destroy. */
int aio_ext_destroy(uint64_t ctx_id);

/* Initialise the AIO enhanced subsystem. */
void aio_enhanced_init(void);

#endif /* AIO_ENHANCED_H */
