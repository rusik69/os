#ifndef LOGBUF_H
#define LOGBUF_H
#include "types.h"
void logbuf_write(const char *msg, uint32_t len);
uint32_t logbuf_read(char *buf, uint32_t max);
uint32_t logbuf_available(void);
#endif
