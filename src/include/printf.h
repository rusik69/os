#ifndef PRINTF_H
#define PRINTF_H

#include "types.h"

int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vkprintf(const char *fmt, __builtin_va_list ap) __attribute__((format(printf, 1, 0)));
void kprintf_set_hook(void (*hook)(char, void *), void *ctx);
void kprintf_get_hook(void (**hook)(char,void*), void **ctx);
void kprintf_set_flush(void (*flush)(void *), void *ctx);
void kprintf_flush(void);
int kprintf_dmesg(char *buf, int max);
void kprintf_dmesg_clear(void);
void kprintf_dmesg_flush_serial(void);
int kprintf_ratelimited(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

int vsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap) __attribute__((format(printf, 3, 0)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
