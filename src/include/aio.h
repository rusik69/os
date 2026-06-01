#ifndef AIO_H
#define AIO_H
#include "types.h"
struct aio_request {
    int fd;
    void *buf;
    size_t count;
    uint64_t offset;
    int is_read;
    int status;
};
void aio_init(void);
int aio_read(int fd, void *buf, size_t count, uint64_t offset);
int aio_write(int fd, const void *buf, size_t count, uint64_t offset);
#endif
