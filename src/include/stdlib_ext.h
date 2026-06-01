#ifndef STDLIB_EXT_H
#define STDLIB_EXT_H

#include "types.h"     /* size_t */

/*
 * Extended stdlib functions.
 *
 * Functions already available via stdlib.h + stdlib.c:
 *   atoi, atol, strtol, strtoul, itoa, ltoa, rand, srand, qsort, bsearch
 *
 * Additional functions declared here (implemented in stdlib_ext.c):
 */

long long atoll(const char *nptr);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
char *ultoa(unsigned long value, char *str, int base);

#endif /* STDLIB_EXT_H */
