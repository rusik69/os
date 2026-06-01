#ifndef FILE_LOCK_H
#define FILE_LOCK_H
#include "types.h"
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
void file_lock_init(void);
int file_lock_set(const char *path, int type);
int file_lock_unlock(const char *path);
#endif
