#ifndef ASSERT_H
#define ASSERT_H

/*
 * Assertion support for kernel code.
 *
 * Usage:
 *   assert(ptr != NULL);
 *   static_assert(sizeof(int) == 4, "unexpected int size");
 */

#include "printf.h"     /* kprintf */

#ifdef __cplusplus
#define static_assert _Static_assert
#else
/* C17 has _Static_assert as a keyword; provide a convenience macro */
#define static_assert _Static_assert
#endif

/* The underlying assertion failure handler (defined in assert.c). */
void __assert_fail(const char *expr, const char *file, int line,
                   const char *func) __attribute__((noreturn));

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((void)(__builtin_expect(!!(expr), 1) || \
            (__assert_fail(#expr, __FILE__, __LINE__, __func__), 0)))
#endif

#endif /* ASSERT_H */
