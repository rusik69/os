#include "fifo.h"
#include "pipe.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

static struct fifo fifo_table[FIFO_MAX];
static int fifo_inited = 0;

void fifo_init(void) {
    memset(fifo_table, 0, sizeof(fifo_table));
    fifo_inited = 1;
}

int fifo_create(const char *path) {
    if (!fifo_inited) return -1;

    /* Check if already exists */
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use && strcmp(fifo_table[i].path, path) == 0)
            return -1;
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (!fifo_table[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* Create a new pipe */
    int pipe_id = pipe_create();
    if (pipe_id < 0) return -1;

    strncpy(fifo_table[slot].path, path, 63);
    fifo_table[slot].path[63] = '\0';
    fifo_table[slot].pipe_id = pipe_id;
    fifo_table[slot].inode = (uint32_t)(0xF1F00000 | (slot & 0xFFFF));
    fifo_table[slot].in_use = 1;

    /* Create a VFS file entry of type FIFO */
    if (vfs_create(path, 1) < 0) {
        /* It might already exist; that's okay if it's our FIFO */
    }

    return 0;
}

int fifo_open(const char *path) {
    if (!fifo_inited) return -1;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use && strcmp(fifo_table[i].path, path) == 0)
            return fifo_table[i].pipe_id;
    }
    return -1;
}

int fifo_is_fifo(const char *path) {
    if (!fifo_inited) return 0;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use && strcmp(fifo_table[i].path, path) == 0)
            return 1;
    }
    return 0;
}

int fifo_unlink(const char *path) {
    if (!fifo_inited) return -1;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use && strcmp(fifo_table[i].path, path) == 0) {
            pipe_close_read(fifo_table[i].pipe_id);
            pipe_close_write(fifo_table[i].pipe_id);
            fifo_table[i].in_use = 0;
            vfs_unlink(path);
            return 0;
        }
    }
    return -1;
}

/* ── fifo_read ────────────────────────────────────────── */
static int fifo_read(int fd, void *buf, size_t count)
{
    (void)fd;
    /* FIFO reads delegate to the underlying pipe */
    if (!fifo_inited) return -EINVAL;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use) {
            return pipe_read(fifo_table[i].pipe_id, buf, (int)count);
        }
    }
    return -EBADF;
}

/* ── fifo_write ───────────────────────────────────────── */
static int fifo_write(int fd, const void *buf, size_t count)
{
    (void)fd;
    if (!fifo_inited) return -EINVAL;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use) {
            return pipe_write(fifo_table[i].pipe_id, buf, (int)count);
        }
    }
    return -EBADF;
}

/* ── fifo_poll ────────────────────────────────────────── */
static int fifo_poll(int fd)
{
    (void)fd;
    if (!fifo_inited) return 0;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use) {
            /* Check if pipe has data to read */
            return pipe_poll(fifo_table[i].pipe_id, 1, NULL);
        }
    }
    return 0;
}

/* ── fifo_ioctl ───────────────────────────────────────── */
static int fifo_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/* ── fifo_release ─────────────────────────────────────── */
static int fifo_release(int fd)
{
    (void)fd;
    if (!fifo_inited) return -EINVAL;
    for (int i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].in_use) {
            /* Close the underlying pipe's read end */
            pipe_close_read(fifo_table[i].pipe_id);
            fifo_table[i].in_use = 0;
            return 0;
        }
    }
    return -EBADF;
}