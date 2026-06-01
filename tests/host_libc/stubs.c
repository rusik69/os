/* stubs.c — Host-side stubs for kernel functions needed by printf.c
 *
 * Compiled WITHOUT kernel headers to avoid type conflicts.
 * Types are hard-coded to match the kernel ABI (LP64: unsigned long long = 64-bit).
 */

#include <stddef.h>

/* ── Kernel heap API ──────────────────────────────────────────────── */
/* Kernel's static inline free()/malloc() in stdlib.h delegate to these. */
void *libc_malloc(unsigned long long size) {
    return __builtin_malloc((unsigned long)size);
}
void libc_free(void *ptr) {
    __builtin_free(ptr);
}
void *kmalloc(unsigned long long size) {
    return __builtin_malloc((unsigned long)size);
}
void kfree(void *ptr) {
    __builtin_free(ptr);
}

/* ── Kernel timer stub ──────────────────────────────────────────── */
unsigned long long timer_get_ticks(void) {
    static unsigned long long fake_tick = 0;
    return fake_tick++;
}

/* ── Serial write stub (printf.c calls this as serial_write(chunk)) ── */
void serial_write(const char *str) {
    (void)str;
}
