/*
 * devfs.c — /dev virtual filesystem
 *
 * Character device nodes: /dev/null, /dev/zero, /dev/random, /dev/kmsg
 * Read-only directory listing; writes to /dev/null are silently discarded.
 * /dev/kmsg supports structured kernel log access (Linux-compatible).
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

/* ── /dev/kmsg helpers ──────────────────────────────────────────── */

/*
 * Parse a Linux /dev/kmsg write buffer of the form "<N>message" where
 * N = facility * 8 + severity.  Returns the parsed severity (0-7), or
 * the default message log level if no valid prefix is found.  Advances
 * *data past the prefix so the caller can log the message text only.
 */
static int kmsg_parse_priority(const char **data, int *out_size) {
    const char *p = *data;
    int remaining = *out_size;

    /* Must start with '<' */
    if (remaining > 0 && *p == '<') {
        p++;
        remaining--;
        int pri = 0;
        /* Collect digits */
        while (remaining > 0 && *p >= '0' && *p <= '9') {
            pri = pri * 10 + (*p - '0');
            p++;
            remaining--;
        }
        /* Must end with '>' */
        if (remaining > 0 && *p == '>') {
            p++;
            remaining--;
            *data = p;
            *out_size = remaining;
            /* Extract severity: pri & 0x07 (lower 3 bits) */
            int sev = pri & 0x07;
            if (sev >= 0 && sev <= 7)
                return sev;
        }
    }
    return default_message_loglevel;
}

/* ── VFS operations ─────────────────────────────────────────────── */

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
    if (strcmp(path, "/dev/kmsg") == 0) {
        /* Return current dmesg buffer content */
        int n = kprintf_dmesg((char *)buf, (int)max_size);
        if (out_size) *out_size = (uint32_t)(n > 0 ? n : 0);
        return 0;
    }
    return -1;
}

static int devfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;
    /* /dev/null silently accepts all writes */
    if (strcmp(path, "/dev/null") == 0) return 0;
    if (strcmp(path, "/dev/kmsg") == 0) {
        /* Parse Linux /dev/kmsg write format: "<N>message" */
        const char *msg = (const char *)data;
        int remaining = (int)size;
        int sev = kmsg_parse_priority(&msg, &remaining);

        /* Trim trailing whitespace/newline */
        while (remaining > 0 && (msg[remaining - 1] == '\n' ||
                                  msg[remaining - 1] == '\r' ||
                                  msg[remaining - 1] == ' '))
            remaining--;

        if (remaining > 0) {
            /* Log with the parsed severity level */
            char stack_buf[256];
            int copy_len = remaining < 255 ? remaining : 255;
            memcpy(stack_buf, msg, (size_t)copy_len);
            stack_buf[copy_len] = '\0';
            kprintf_level(sev, "[kmsg] %s\n", stack_buf);
        }
        return (int)size;
    }
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
    if (strcmp(path, "/dev/kmsg") == 0) {
        st->type = 1;           /* character device */
        st->size = (uint32_t)kprintf_dmesg_used();
        return 0;
    }
    return -1;
}

static int devfs_readdir(void *priv, const char *path) {
    (void)priv;
    if (strcmp(path, "/dev") != 0) return -1;
    kprintf("null\nzero\nrandom\nkmsg\n");
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
