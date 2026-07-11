#include "logbuf.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#define LOGBUF_SIZE 32768
static char log_buffer[LOGBUF_SIZE];
static uint32_t log_head = 0, log_tail = 0;
static spinlock_t logbuf_lock = SPINLOCK_INIT;
void logbuf_write(const char *msg, uint32_t len) {
    uint64_t flags;
    if (!msg || len == 0) return;
    /* Truncate oversized writes to preserve at least half the buffer
     * for readers; prevents a single large write from consuming the
     * entire ring and guarantees forward progress for concurrent
     * readers. */
    if (len > LOGBUF_SIZE / 2) len = LOGBUF_SIZE / 2;
    spinlock_irqsave_acquire(&logbuf_lock, &flags);
    for (uint32_t i = 0; i < len; i++) {
        log_buffer[log_head] = msg[i];
        log_head = (log_head + 1) % LOGBUF_SIZE;
        if (log_head == log_tail) {
            log_tail = (log_tail + 1) % LOGBUF_SIZE;
        }
    }
    spinlock_irqsave_release(&logbuf_lock, flags);
}
uint32_t logbuf_read(char *buf, uint32_t max) {
    uint64_t flags;
    uint32_t count = 0;
    spinlock_irqsave_acquire(&logbuf_lock, &flags);
    while (log_tail != log_head && count < max) {
        buf[count++] = log_buffer[log_tail];
        log_tail = (log_tail + 1) % LOGBUF_SIZE;
    }
    spinlock_irqsave_release(&logbuf_lock, flags);
    if (count > 0 && buf[count-1] != '\n') buf[count++] = '\n';
    return count;
}
uint32_t logbuf_available(void) {
    uint64_t flags;
    uint32_t avail;
    spinlock_irqsave_acquire(&logbuf_lock, &flags);
    if (log_head >= log_tail)
        avail = log_head - log_tail;
    else
        avail = LOGBUF_SIZE - (log_tail - log_head);
    spinlock_irqsave_release(&logbuf_lock, flags);
    return avail;
}