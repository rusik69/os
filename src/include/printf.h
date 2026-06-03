#ifndef PRINTF_H
#define PRINTF_H

#include "types.h"

/* Kernel log levels (Linux-compatible) */
#define KERN_EMERG   0   /* system is unusable */
#define KERN_ALERT   1   /* action must be taken immediately */
#define KERN_CRIT    2   /* critical conditions */
#define KERN_ERR     3   /* error conditions */
#define KERN_WARNING 4   /* warning conditions */
#define KERN_NOTICE  5   /* normal but significant condition */
#define KERN_INFO    6   /* informational */
#define KERN_DEBUG   7   /* debug-level messages */

/* Default console log level (only messages <= this level are printed) */
extern int console_loglevel;
/* Default message log level (used when no explicit level given) */
extern int default_message_loglevel;

int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vkprintf(const char *fmt, __builtin_va_list ap) __attribute__((format(printf, 1, 0)));
int kprintf_level(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Low-level character output */
void kputchar(char c);

void kprintf_set_hook(void (*hook)(char, void *), void *ctx);
void kprintf_get_hook(void (**hook)(char,void*), void **ctx);
void kprintf_set_flush(void (*flush)(void *), void *ctx);
void kprintf_flush(void);
int kprintf_dmesg(char *buf, int max);
void kprintf_dmesg_clear(void);
void kprintf_dmesg_flush_serial(void);
int kprintf_dmesg_used(void);       /* bytes currently in dmesg buffer */
int kprintf_ratelimited(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* dmesg ring buffer configuration */
void dmesg_resize(int new_size);
void dmesg_clear(void);
int dmesg_get_size(void);

int vsnprintf(char *buf, size_t n, const char *fmt, __builtin_va_list ap) __attribute__((format(printf, 3, 0)));
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
