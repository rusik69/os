#ifndef PRINTF_H
#define PRINTF_H

#include "types.h"

int kprintf(const char *fmt, ...);
void kprintf_set_hook(void (*hook)(char, void *), void *ctx);
void kprintf_get_hook(void (**hook)(char,void*), void **ctx);
void kprintf_set_flush(void (*flush)(void *), void *ctx);
void kprintf_flush(void);
int kprintf_dmesg(char *buf, int max);

#endif
