#include "assert.h"
#include "printf.h"

/*
 * __assert_fail — called by the assert() macro when the expression is false.
 * Prints a diagnostic message and halts the system.
 */
void __assert_fail(const char *expr, const char *file, int line,
                   const char *func) {
    kprintf("Assertion failed: %s, function %s, file %s, line %d\n",
            expr, func ? func : "?", file, line);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ── Stub: assert_handler ─────────────────────────────── */
int assert_handler(const char *expr, const char *file, int line, const char *func)
{
    (void)expr;
    (void)file;
    (void)line;
    (void)func;
    kprintf("[assert] assert_handler: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: assert_set_handler ─────────────────────────────── */
int assert_set_handler(void *handler)
{
    (void)handler;
    kprintf("[assert] assert_set_handler: not yet implemented\n");
    return -ENOSYS;
}
