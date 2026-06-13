#ifndef DHCP_H
#define DHCP_H
#include "types.h"
void dhcp_init(void);
int dhcp_discover(void);
void dhcp_set_server(uint32_t ip);
uint32_t dhcp_get_server(void);

/* DHCP relay agent (RFC 3046) */
void dhcp_relay_enable(uint32_t server_ip, uint32_t giaddr);
void dhcp_relay_disable(void);
int  dhcp_relay_is_active(void);
void dhcp_relay_set_circuit_id(const uint8_t *data, int len);
void dhcp_relay_set_remote_id(const uint8_t *data, int len);
int  dhcp_relay_forward(const uint8_t *pkt, int len, int from_port);

/* DHCPv6 prefix delegation (RFC 3633) */
int  dhcpv6_pd_solicit(void);
int  dhcpv6_pd_is_active(void);
int  dhcpv6_pd_get_prefix(uint8_t *prefix_out, uint8_t *length_out);
#endif
