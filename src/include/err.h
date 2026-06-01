#ifndef ERR_H
#define ERR_H

#include "types.h"
#include "errno.h"

/*
 * Kernel error pointer encoding.
 *
 * Pointers with values in the range [-MAX_ERRNO, -1] (i.e. the last page of
 * the address space) are reserved for encoding error codes.
 */

#define MAX_ERRNO 4095
#define ERR_PTR_MASK ~(uintptr_t)(MAX_ERRNO)

static inline void *ERR_PTR(int error) {
    return (void *)(uintptr_t)(-error);
}

static inline int PTR_ERR(const void *ptr) {
    return -(int)(uintptr_t)ptr;
}

static inline int IS_ERR(const void *ptr) {
    return (uintptr_t)ptr >= (uintptr_t)-MAX_ERRNO;
}

static inline int IS_ERR_OR_NULL(const void *ptr) {
    return !ptr || IS_ERR(ptr);
}

/* Encode a negative error into a pointer, or return the pointer if no error */
static inline void *ERR_CAST(const void *ptr) {
    return (void *)ptr;
}

/* Return a pointer after encoding an error code */
#define ERR_PTR_OR_NULL(ptr) (IS_ERR(ptr) ? NULL : (void *)(ptr))

#endif /* ERR_H */
