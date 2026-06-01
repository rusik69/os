#ifndef ERRNIO_EXT_H
#define ERRNIO_EXT_H

#include "errno.h"
#include "printf.h"    /* for perror */

/*
 * Extended errno support.
 *
 * errno.h provides the numeric error constants (EPERM, ENOENT, …).
 * This header adds string descriptions and a perror facility.
 */

/* Thread-local / global errno storage. */
extern int __errno_value;
#define errno __errno_value
int *__errno_location(void);

/* Return a string description for an error number. */
char *strerror(int errnum);

/* Print "s: error-description" to the kernel log. */
void perror(const char *s);

#endif /* ERRNIO_EXT_H */
