#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "trace.h"
#include "string.h"
#define TRACE_BUF_SIZE 4096
static char trace_buf[TRACE_BUF_SIZE];
static int trace_pos = 0;
void trace_init(void) {
    memset(trace_buf, 0, sizeof(trace_buf));
    kprintf("[OK] Kernel tracing initialized\n");
}
void trace_write(const char *msg) {
    if (!msg) return;
    while (*msg && trace_pos < TRACE_BUF_SIZE - 1) trace_buf[trace_pos++] = *msg++;
    trace_buf[trace_pos] = '\0';
}
void trace_dump(void) {
    kprintf("=== TRACE DUMP ===\n%s\n=== END TRACE ===\n", trace_buf);
}
