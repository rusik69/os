#ifndef PRINTF_H
#define PRINTF_H

#include "types.h"

int kprintf(const char *fmt, ...);
void kprintf_set_hook(void (*hook)(char, void *), void *ctx);
void kprintf_get_hook(void (**hook)(char,void*), void **ctx);
void kprintf_set_flush(void (*flush)(void *), void *ctx);
void kprintf_flush(void);
int kprintf_dmesg(char *buf, int max);

int vsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);

#endif
