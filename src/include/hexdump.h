#ifndef HEXDUMP_H
#define HEXDUMP_H
#include "types.h"
void print_hex_dump(const char *prefix, const void *buf, uint32_t len);
#endif
