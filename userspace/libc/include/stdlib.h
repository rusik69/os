#ifndef _STDLIB_H
#define _STDLIB_H

#include "unistd.h"

/* Memory allocation */
void *malloc(unsigned long size);
void free(void *ptr);
void *calloc(unsigned long nmemb, unsigned long size);
void *realloc(void *ptr, unsigned long size);

/* String conversion */
int atoi(const char *s);

/* Process control */
void abort(void);

#endif /* _STDLIB_H */
