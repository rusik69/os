#include "memfd.h"
#include "string.h"
#include "kernel.h"
#include "heap.h"
#include "printf.h"
#include "errno.h"

static struct memfd memfd_table[MEMFD_MAX];
static int memfd_initialised = 0;

void memfd_init(void)
{
    if (memfd_initialised)
        return;

    for (int i = 0; i < MEMFD_MAX; i++) {
        struct memfd *mfd = &memfd_table[i];
        spinlock_init(&mfd->lock);
        mfd->used = 0;
        mfd->data = NULL;
        mfd->size = 0;
        mfd->seals = 0;
        mfd->refcount = 0;
        mfd->fd = -1;
        mfd->flags = 0;
        memset(mfd->name, 0, MEMFD_NAME_MAX);
    }
    memfd_initialised = 1;
    kprintf("memfd: initialised with %d slots (MFD_ALLOW_SEALING + MFD_HUGETLB support)\n",
            MEMFD_MAX);
}

static int memfd_find_free(void)
{
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (!memfd_table[i].used)
            return i;
    }
    return -EMFILE;
}

int memfd_create(const char *name, int flags)
{
    /* Validate flags: reject unknown bits (MFD_CLOEXEC, MFD_ALLOW_SEALING,
     * and MFD_HUGETLB are the only recognised flags).  MFD_HUGETLB is
     * accepted for Linux compatibility but uses kmalloc backing for now;
     * full huge-page backing can be added when needed. */
    int known_flags = MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB;
    if (flags & ~known_flags)
        return -EINVAL;

    int fd = memfd_find_free();
    if (fd < 0)
        return fd;

    struct memfd *mfd = &memfd_table[fd];
    spinlock_acquire(&mfd->lock);

    strncpy(mfd->name, name, MEMFD_NAME_MAX - 1);
    mfd->name[MEMFD_NAME_MAX - 1] = '\0';
    mfd->size = 0;
    mfd->data = NULL;
    mfd->seals = 0;
    mfd->refcount = 1;
    mfd->used = 1;
    mfd->flags = flags;
    mfd->fd = MEMFD_BASE + fd;

    spinlock_release(&mfd->lock);
    return mfd->fd;
}

struct memfd *memfd_get(int slot)
{
    if (slot < 0 || slot >= MEMFD_MAX)
        return NULL;

    struct memfd *mfd = &memfd_table[slot];
    spinlock_acquire(&mfd->lock);
    if (!mfd->used) {
        spinlock_release(&mfd->lock);
        return NULL;
    }
    mfd->refcount++;
    spinlock_release(&mfd->lock);
    return mfd;
}

void memfd_put(struct memfd *mfd)
{
    if (!mfd)
        return;

    spinlock_acquire(&mfd->lock);
    mfd->refcount--;
    if (mfd->refcount == 0) {
        if (mfd->data) {
            kfree(mfd->data);
            mfd->data = NULL;
        }
        mfd->size = 0;
        mfd->used = 0;
        mfd->seals = 0;
        mfd->fd = -1;
        mfd->flags = 0;
        memset(mfd->name, 0, MEMFD_NAME_MAX);
    }
    spinlock_release(&mfd->lock);
}

/* Lookup memfd by file descriptor number. */
struct memfd *memfd_get_by_fd(int fd)
{
    if (!memfd_is_fd(fd))
        return NULL;
    int slot = fd - MEMFD_BASE;
    if (slot < 0 || slot >= MEMFD_MAX)
        return NULL;
    return memfd_get(slot);
}

/* Syscall entry: create a memfd and return a file descriptor. */
int memfd_syscall_create(const char *name, unsigned int flags)
{
    return memfd_create(name, (int)flags);
}

/* Read/write at offset within a memfd (by fd number). */
int64_t memfd_read_fd(int fd, void *buf, uint64_t count, uint64_t offset)
{
    struct memfd *mfd = memfd_get_by_fd(fd);
    if (!mfd)
        return -EBADF;
    int64_t ret = memfd_read(mfd, buf, count, offset);
    memfd_put(mfd);
    return ret;
}

int64_t memfd_write_fd(int fd, const void *buf, uint64_t count, uint64_t offset)
{
    struct memfd *mfd = memfd_get_by_fd(fd);
    if (!mfd)
        return -EBADF;
    int64_t ret = memfd_write(mfd, buf, count, offset);
    memfd_put(mfd);
    return ret;
}

int64_t memfd_read(struct memfd *mfd, void *buf, uint64_t count, uint64_t offset)
{
    if (!mfd || !buf)
        return -EFAULT;

    spinlock_acquire(&mfd->lock);

    if (!mfd->used) {
        spinlock_release(&mfd->lock);
        return -EBADF;
    }

    if (!mfd->data || offset >= mfd->size) {
        spinlock_release(&mfd->lock);
        return 0;
    }

    uint64_t readable = mfd->size - offset;
    if (count > readable)
        count = readable;

    memcpy(buf, mfd->data + offset, (size_t)count);
    spinlock_release(&mfd->lock);
    return (int64_t)count;
}

int64_t memfd_write(struct memfd *mfd, const void *buf, uint64_t count, uint64_t offset)
{
    if (!mfd || !buf)
        return -EFAULT;

    spinlock_acquire(&mfd->lock);

    if (!mfd->used) {
        spinlock_release(&mfd->lock);
        return -EBADF;
    }

    /* MEMFD_SEAL_WRITE prevents any write */
    if (mfd->seals & MEMFD_SEAL_WRITE) {
        spinlock_release(&mfd->lock);
        return -EPERM;
    }

    uint64_t needed = offset + count;

    if (needed > mfd->size) {
        /* MEMFD_SEAL_FUTURE_WRITE: prevents writes that would require
         * extending the backing store (i.e., allocate new pages).
         * Unlike F_SEAL_GROW, ftruncate expansion can still succeed. */
        if (mfd->seals & MEMFD_SEAL_FUTURE_WRITE) {
            spinlock_release(&mfd->lock);
            return -EPERM;
        }

        /* MEMFD_SEAL_GROW prevents expanding the size */
        if (mfd->seals & MEMFD_SEAL_GROW) {
            spinlock_release(&mfd->lock);
            return -EPERM;
        }
        /* Grow the backing buffer. */
        uint8_t *new_data = (uint8_t *)kmalloc(needed);
        if (!new_data) {
            spinlock_release(&mfd->lock);
            return -ENOMEM;
        }
        if (mfd->data) {
            memcpy(new_data, mfd->data, mfd->size);
            kfree(mfd->data);
        }
        memset(new_data + mfd->size, 0, needed - mfd->size);
        mfd->data = new_data;
        mfd->size = needed;
    }

    memcpy(mfd->data + offset, buf, count);
    spinlock_release(&mfd->lock);
    return (int64_t)count;
}

int memfd_add_seal(struct memfd *mfd, int seal)
{
    if (!mfd)
        return -EINVAL;

    spinlock_acquire(&mfd->lock);

    if (!mfd->used) {
        spinlock_release(&mfd->lock);
        return -EBADF;
    }

    /* MFD_ALLOW_SEALING must have been set at creation time; otherwise
     * sealing is prohibited (Linux-compatible behaviour). */
    if (!(mfd->flags & MFD_ALLOW_SEALING)) {
        spinlock_release(&mfd->lock);
        return -EPERM;
    }

    /* MEMFD_SEAL_SEAL is irreversible — once set, no further seals can be added. */
    if (mfd->seals & MEMFD_SEAL_SEAL) {
        spinlock_release(&mfd->lock);
        return -EPERM;
    }

    mfd->seals |= seal;
    spinlock_release(&mfd->lock);
    return 0;
}

int memfd_add_seals_fd(int fd, int seals)
{
    struct memfd *mfd = memfd_get_by_fd(fd);
    if (!mfd)
        return -EBADF;
    int ret = memfd_add_seal(mfd, seals);
    memfd_put(mfd);
    return ret;
}

int memfd_get_seals_fd(int fd)
{
    struct memfd *mfd = memfd_get_by_fd(fd);
    if (!mfd)
        return -EBADF;
    int seals = memfd_get_seals(mfd);
    memfd_put(mfd);
    return seals;
}

int memfd_get_seals(struct memfd *mfd)
{
    if (!mfd)
        return -EINVAL;

    spinlock_acquire(&mfd->lock);
    int seals = mfd->seals;
    spinlock_release(&mfd->lock);
    return seals;
}

uint64_t memfd_get_size(struct memfd *mfd)
{
    if (!mfd)
        return 0;

    spinlock_acquire(&mfd->lock);
    uint64_t sz = mfd->size;
    spinlock_release(&mfd->lock);
    return sz;
}

int memfd_set_size(struct memfd *mfd, uint64_t new_size)
{
    if (!mfd)
        return -EINVAL;

    spinlock_acquire(&mfd->lock);

    if (!mfd->used) {
        spinlock_release(&mfd->lock);
        return -EBADF;
    }

    if (new_size < mfd->size && (mfd->seals & MEMFD_SEAL_SHRINK)) {
        spinlock_release(&mfd->lock);
        return -EPERM;
    }
    if (new_size > mfd->size && (mfd->seals & MEMFD_SEAL_GROW)) {
        spinlock_release(&mfd->lock);
        return -EPERM;
    }

    if (new_size == 0) {
        if (mfd->data) {
            kfree(mfd->data);
            mfd->data = NULL;
        }
        mfd->size = 0;
    } else if (new_size != mfd->size) {
        uint8_t *new_data = (uint8_t *)kmalloc(new_size);
        if (!new_data) {
            spinlock_release(&mfd->lock);
            return -ENOMEM;
        }
        uint64_t copy_sz = (new_size < mfd->size) ? new_size : mfd->size;
        if (mfd->data) {
            memcpy(new_data, mfd->data, copy_sz);
            kfree(mfd->data);
        }
        if (new_size > mfd->size)
            memset(new_data + mfd->size, 0, new_size - mfd->size);
        mfd->data = new_data;
        mfd->size = new_size;
    }

    spinlock_release(&mfd->lock);
    return 0;
}

/* ── Stub: memfd_seal ─────────────────────────────── */
int memfd_seal(int fd, unsigned int seals)
{
    (void)fd;
    (void)seals;
    kprintf("[memfd] memfd_seal: not yet implemented\n");
    return -ENOSYS;
}
