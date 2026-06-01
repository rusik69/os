#ifndef XATTR_H
#define XATTR_H
#include "types.h"
void xattr_init(void);
int xattr_set(const char *path, const char *name, const void *value, size_t size);
int xattr_get(const char *path, const char *name, void *value, size_t *size);
#endif
