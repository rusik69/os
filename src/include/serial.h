#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putchar(char c);
void serial_write(const char *str);
int  serial_readable(void);
char serial_getchar(void);
void serial_read_line(char *buf, int max);

#endif
