#ifndef MEMFD_H
#define MEMFD_H

#include "types.h"
#include "spinlock.h"

#define MEMFD_MAX 32
#define MEMFD_NAME_MAX 32

/* Seal flags */
#define MEMFD_SEAL_SHRINK 1
#define MEMFD_SEAL_GROW   2
#define MEMFD_SEAL_WRITE  4
#define MEMFD_SEAL_SEAL   8

struct memfd {
    char name[MEMFD_NAME_MAX];
    uint64_t size;
    uint8_t *data;
    spinlock_t lock;
    int seals;
    int refcount;
    int used; /* 1 if this slot is active */
};

/* Create a new memfd with given name and flags. Returns fd index or -errno. */
int memfd_create(const char *name, int flags);

/* Get/put reference on a memfd by fd index. */
struct memfd *memfd_get(int fd);
void memfd_put(struct memfd *mfd);

/* Read/write at offset within a memfd. */
int64_t memfd_read(struct memfd *mfd, void *buf, uint64_t count, uint64_t offset);
int64_t memfd_write(struct memfd *mfd, const void *buf, uint64_t count, uint64_t offset);

/* Seal operations. */
int memfd_add_seal(struct memfd *mfd, int seal);
int memfd_get_seals(struct memfd *mfd);

/* Size operations. */
uint64_t memfd_get_size(struct memfd *mfd);
int memfd_set_size(struct memfd *mfd, uint64_t new_size);

/* Initialise the memfd subsystem. */
void memfd_init(void);

#endif /* MEMFD_H */
