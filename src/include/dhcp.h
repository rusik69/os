#ifndef DHCP_H
#define DHCP_H
#include "types.h"
void dhcp_init(void);
int dhcp_discover(void);
void dhcp_set_server(uint32_t ip);
uint32_t dhcp_get_server(void);
#endif
