#ifndef E1000_H
#define E1000_H

#include "types.h"

#define E1000_VENDOR  0x8086
#define E1000_DEVICE  0x100E  /* 82540EM - QEMU default */

int e1000_init(void);
int e1000_send(const void *data, uint16_t len);
int e1000_receive(void *buf, uint16_t max_len);
void e1000_get_mac(uint8_t *mac);
int e1000_is_present(void);

#endif
