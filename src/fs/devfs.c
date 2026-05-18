/*
 * devfs.c — /dev virtual filesystem
 *
 * Character device nodes: /dev/null, /dev/zero, /dev/random
 * Read-only directory listing; writes to /dev/null are silently discarded.
 */

#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "types.h"

/* Simple pseudo-random LCG for /dev/random */
static uint32_t rand_state = 0xDEADBEEF;
static uint8_t dev_rand_byte(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return (uint8_t)(rand_state >> 16);
}

static int devfs_read(void *priv, const char *path, void *buf_v,
                      uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    uint8_t *buf = (uint8_t *)buf_v;

    if (strcmp(path, "/dev/null") == 0) {
        /* reads return 0 bytes (EOF) */
        if (out_size) *out_size = 0;
        return 0;
    }
    if (strcmp(path, "/dev/zero") == 0) {
        memset(buf, 0, max_size);
        if (out_size) *out_size = max_size;
        return 0;
    }
    if (strcmp(path, "/dev/random") == 0) {
        for (uint32_t i = 0; i < max_size; i++)
            buf[i] = dev_rand_byte();
        if (out_size) *out_size = max_size;
        return 0;
    }
    return -1;
}

static int devfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv; (void)data; (void)size;
    /* /dev/null silently accepts all writes */
    if (strcmp(path, "/dev/null") == 0) return 0;
    return -1;
}

static int devfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    if (strcmp(path, "/dev") == 0) {
        st->type = 2; st->size = 0; return 0;
    }
    if (strcmp(path, "/dev/null")   == 0 ||
        strcmp(path, "/dev/zero")   == 0 ||
        strcmp(path, "/dev/random") == 0) {
        st->type = 1; st->size = 0; return 0;
    }
    return -1;
}

static int devfs_readdir(void *priv, const char *path) {
    (void)priv;
    if (strcmp(path, "/dev") != 0) return -1;
    kprintf("null\nzero\nrandom\n");
    return 0;
}

struct vfs_ops devfs_ops = {
    .read    = devfs_read,
    .write   = devfs_write,
    .stat    = devfs_stat,
    .create  = NULL,
    .unlink  = NULL,
    .readdir = devfs_readdir,
};
