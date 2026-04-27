#include "pipe.h"
#include "scheduler.h"
#include "string.h"

static struct pipe pipe_table[PIPE_MAX];

void pipe_init(void) {
    memset(pipe_table, 0, sizeof(pipe_table));
}

int pipe_create(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipe_table[i].in_use) {
            pipe_table[i].read_pos  = 0;
            pipe_table[i].write_pos = 0;
            pipe_table[i].count     = 0;
            pipe_table[i].readers   = 1;
            pipe_table[i].writers   = 1;
            pipe_table[i].in_use    = 1;
            return i;
        }
    }
    return -1;
}

int pipe_write(int pipe_id, const void *buf, int len) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;

    struct pipe *p = &pipe_table[pipe_id];
    if (p->readers == 0) return -1;  /* broken pipe */

    const uint8_t *src = (const uint8_t *)buf;
    int written = 0;

    while (written < len) {
        /* Block while full */
        while (p->count == PIPE_BUF_SIZE) {
            if (p->readers == 0) return written ? written : -1;
            scheduler_yield();
        }

        int space = PIPE_BUF_SIZE - p->count;
        int to_write = len - written;
        if (to_write > space) to_write = space;

        for (int i = 0; i < to_write; i++) {
            p->buf[p->write_pos] = src[written + i];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        }
        p->count  += to_write;
        written   += to_write;
    }
    return written;
}

int pipe_read(int pipe_id, void *buf, int len) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return -1;

    struct pipe *p = &pipe_table[pipe_id];

    /* Block while empty and there are still writers */
    while (p->count == 0) {
        if (p->writers == 0) return 0;  /* EOF */
        scheduler_yield();
    }

    uint8_t *dst = (uint8_t *)buf;
    int to_read = (p->count < len) ? p->count : len;

    for (int i = 0; i < to_read; i++) {
        dst[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
    }
    p->count -= to_read;
    return to_read;
}

void pipe_close_read(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    struct pipe *p = &pipe_table[pipe_id];
    if (p->readers > 0) p->readers--;
    if (p->readers == 0 && p->writers == 0) p->in_use = 0;
}

void pipe_close_write(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    struct pipe *p = &pipe_table[pipe_id];
    if (p->writers > 0) p->writers--;
    if (p->readers == 0 && p->writers == 0) p->in_use = 0;
}

int pipe_available(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipe_table[pipe_id].in_use)
        return 0;
    return pipe_table[pipe_id].count;
}
