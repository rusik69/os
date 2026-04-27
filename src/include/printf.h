#ifndef PRINTF_H
#define PRINTF_H

#include "types.h"

int kprintf(const char *fmt, ...);
void kprintf_set_hook(void (*hook)(char, void *), void *ctx);

#endif
