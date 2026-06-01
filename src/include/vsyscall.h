#ifndef VSYSCALL_H
#define VSYSCALL_H

#include "types.h"

/* Initialize the vsyscall page (VDSO-like) at a fixed address */
int vsyscall_init(void);

/* Get the vsyscall page virtual address */
void *vsyscall_get_page(void);

/* vsyscall entry points */
void vsyscall_gettimeofday(void);
void vsyscall_time(void);
void vsyscall_getcpu(void);

#endif /* VSYSCALL_H */
