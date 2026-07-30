#include "logbuf.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#define LOGBUF_SIZE 32768
static char log_buffer[LOGBUF_SIZE];
static uint32_t log_head = 0, log_tail = 0;

#ifdef LOGBUF_HOST_TEST
/* Host-mode tests: use a simple volatile flag instead of
 * spinlock_irqsave_acquire/release which contain the 'cli' instruction
 * (privileged, crashes in userspace). */
static volatile int logbuf_lock = 0;
#define LOGBUF_LOCK_SCOPE_BEGIN \
    do { \
        while (__sync_lock_test_and_set(&logbuf_lock, 1)) { __asm__ volatile("pause"); } \
        __sync_synchronize();
#define LOGBUF_LOCK_SCOPE_END \
        __sync_synchronize(); \
        __sync_lock_release(&logbuf_lock); \
    } while (0)
#else
static spinlock_t logbuf_lock = SPINLOCK_INIT;
#define LOGBUF_LOCK_SCOPE_BEGIN \
    do { \
        uint64_t _lf; \
        spinlock_irqsave_acquire(&logbuf_lock, &_lf);
#define LOGBUF_LOCK_SCOPE_END \
        spinlock_irqsave_release(&logbuf_lock, _lf); \
    } while (0)
#endif

void logbuf_write(const char *msg, uint32_t len) {
    if (!msg || len == 0) return;
    /* Truncate oversized writes to preserve at least half the buffer
     * for readers; prevents a single large write from consuming the
     * entire ring and guarantees forward progress for concurrent
     * readers. */
    if (len > LOGBUF_SIZE / 2) len = LOGBUF_SIZE / 2;
    LOGBUF_LOCK_SCOPE_BEGIN
    for (uint32_t i = 0; i < len; i++) {
        log_buffer[log_head] = msg[i];
        log_head = (log_head + 1) % LOGBUF_SIZE;
        if (log_head == log_tail) {
            log_tail = (log_tail + 1) % LOGBUF_SIZE;
        }
    }
    LOGBUF_LOCK_SCOPE_END;
}
uint32_t logbuf_read(char *buf, uint32_t max) {
    uint32_t count = 0;
    LOGBUF_LOCK_SCOPE_BEGIN
    while (log_tail != log_head && count < max) {
        buf[count++] = log_buffer[log_tail];
        log_tail = (log_tail + 1) % LOGBUF_SIZE;
    }
    LOGBUF_LOCK_SCOPE_END;
    if (count > 0 && buf[count-1] != '\n') {
        /* Only append newline if there's room in the buffer.
         * Prevents out-of-bounds write when count == max. */
        if (count < max)
            buf[count++] = '\n';
    }
    return count;
}
uint32_t logbuf_available(void) {
    uint32_t avail;
    LOGBUF_LOCK_SCOPE_BEGIN
    if (log_head >= log_tail)
        avail = log_head - log_tail;
    else
        avail = LOGBUF_SIZE - (log_tail - log_head);
    LOGBUF_LOCK_SCOPE_END;
    return avail;
}