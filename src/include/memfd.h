#ifndef MEMFD_H
#define MEMFD_H

#include "types.h"
#include "spinlock.h"

#define MEMFD_MAX 32
#define MEMFD_NAME_MAX 32

/* memfd_create flags (Linux-compatible) */
#define MFD_CLOEXEC       (1 << 0)
#define MFD_ALLOW_SEALING (1 << 1)
#define MFD_HUGETLB       (1 << 2)

/* Seal flags (Linux-compatible) */
#define MEMFD_SEAL_SHRINK       1
#define MEMFD_SEAL_GROW         2
#define MEMFD_SEAL_WRITE        4
#define MEMFD_SEAL_SEAL         8
#define MEMFD_SEAL_FUTURE_WRITE 16 /* future writes via mmap page faults are blocked;
                                      write() to already-committed pages still allowed */

/* fcntl commands for memfd seal operations */
#define F_ADD_SEALS  1033  /* Add seal bitmask to memfd */
#define F_GET_SEALS  1034  /* Get current seal bitmask from memfd */

/* FD range for memfd file descriptors */
#define MEMFD_BASE    800
#define MEMFD_FD_MAX  (MEMFD_BASE + MEMFD_MAX)

struct memfd {
    char name[MEMFD_NAME_MAX];
    uint64_t size;
    uint8_t *data;
    spinlock_t lock;
    int seals;
    int refcount;
    int used;   /* 1 if this slot is active */
    int fd;     /* file descriptor number for this memfd */
    int flags;  /* creation flags (MFD_CLOEXEC, MFD_ALLOW_SEALING, MFD_HUGETLB) */
};

/* Create a new memfd with given name and flags. Returns fd index or -errno. */
int memfd_create(const char *name, int flags);

/* Get/put reference on a memfd by fd index (internal table index, not fd number). */
struct memfd *memfd_get(int slot);
void memfd_put(struct memfd *mfd);

/* Lookup memfd by file descriptor number. */
struct memfd *memfd_get_by_fd(int fd);

/* Check if an fd is a memfd. */
static inline int memfd_is_fd(int fd) {
    return (fd >= MEMFD_BASE && fd < MEMFD_FD_MAX);
}

/* Syscall entry: create a memfd and return a file descriptor. */
int memfd_syscall_create(const char *name, unsigned int flags);

/* Read/write at offset within a memfd (by fd number). */
int64_t memfd_read_fd(int fd, void *buf, uint64_t count, uint64_t offset);
int64_t memfd_write_fd(int fd, const void *buf, uint64_t count, uint64_t offset);

/* Internal read/write at offset within a memfd struct. */
int64_t memfd_read(struct memfd *mfd, void *buf, uint64_t count, uint64_t offset);
int64_t memfd_write(struct memfd *mfd, const void *buf, uint64_t count, uint64_t offset);

/* Seal operations. */
int memfd_add_seal(struct memfd *mfd, int seal);
int memfd_get_seals(struct memfd *mfd);

/* Add seals by file descriptor. */
int memfd_add_seals_fd(int fd, int seals);
int memfd_get_seals_fd(int fd);

/* Size operations. */
uint64_t memfd_get_size(struct memfd *mfd);
int memfd_set_size(struct memfd *mfd, uint64_t new_size);

/* Initialise the memfd subsystem. */
void memfd_init(void);

#endif /* MEMFD_H */
