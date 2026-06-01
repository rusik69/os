#include "logbuf.h"
#include "printf.h"
#include "string.h"
#define LOGBUF_SIZE 32768
static char log_buffer[LOGBUF_SIZE];
static uint32_t log_head = 0, log_tail = 0;
static int log_wrapped = 0;
void logbuf_write(const char *msg, uint32_t len) {
    if (!msg || len == 0) return;
    if (len > LOGBUF_SIZE / 2) len = LOGBUF_SIZE / 2;
    for (uint32_t i = 0; i < len; i++) {
        log_buffer[log_head] = msg[i];
        log_head = (log_head + 1) % LOGBUF_SIZE;
        if (log_head == log_tail) {
            log_tail = (log_tail + 1) % LOGBUF_SIZE;
            log_wrapped = 1;
        }
    }
}
uint32_t logbuf_read(char *buf, uint32_t max) {
    uint32_t count = 0;
    while (log_tail != log_head && count < max) {
        buf[count++] = log_buffer[log_tail];
        log_tail = (log_tail + 1) % LOGBUF_SIZE;
    }
    if (count > 0 && buf[count-1] != '\n') buf[count++] = '\n';
    return count;
}
uint32_t logbuf_available(void) {
    if (log_head >= log_tail) return log_head - log_tail;
    return LOGBUF_SIZE - (log_tail - log_head);
}
