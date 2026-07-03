#ifndef DHCP_H
#define DHCP_H
#include "types.h"
#include "net.h"
#include "netdevice.h"

void dhcp_init(void);
int dhcp_discover(void);
int dhcp_renew(void);
int dhcp_has_lease_func(void);
uint32_t dhcp_get_lease_time(void);
void dhcp_set_server(uint32_t ip);
uint32_t dhcp_get_server(void);
int dhcp_process_timers(void);

/* Device-aware DHCP send functions */
int dhcp_send_discover(struct net_device *dev);
int dhcp_send_request(struct net_device *dev, uint32_t offered_ip);

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

/* ── DHCPv6 client (RFC 8415) ──────────────────────────────────── */

#define DHCPV6_CLIENT_PORT 546
#define DHCPV6_SERVER_PORT 547

/* DHCPv6 message types (RFC 8415 §14) */
#define DHCPV6_SOLICIT    1
#define DHCPV6_ADVERTISE  2
#define DHCPV6_REQUEST    3
#define DHCPV6_REPLY      7

/* DHCPv6 option codes (RFC 8415 §21) */
#define DHCPV6_OPTION_CLIENTID     1
#define DHCPV6_OPTION_SERVERID     2
#define DHCPV6_OPTION_IA_NA        3
#define DHCPV6_OPTION_IA_ADDR      5
#define DHCPV6_OPTION_ORO          6
#define DHCPV6_OPTION_ELAPSED_TIME 8
#define DHCPV6_OPTION_STATUS_CODE 13
#define DHCPV6_OPTION_RAPID_COMMIT 14

/* DUID types (RFC 8415 §11) */
#define DUID_LLT 1
#define DUID_EN  2
#define DUID_LL  3
#define DUID_UUID 4

/* DHCPv6 status codes (RFC 8415 §21.13) */
#define DHCPV6_STATUS_SUCCESS      0
#define DHCPV6_STATUS_NOADDRSAVAIL 2

/* DUID-LL (Link-Layer, type 3): 8 bytes + MAC length
 *   type(2) + hw_type(2) + mac(6) = 10 bytes for Ethernet */
#define DHCPV6_DUID_LL_LEN 10

/* IA_NA option size (without IA_ADDR sub-option) */
#define DHCPV6_IA_NA_HDR_LEN 12    /* IAID(4) + T1(4) + T2(4) */
#define DHCPV6_IA_ADDR_LEN   24    /* addr(16) + pref_lifetime(4) + valid_lifetime(4) */

/* DHCPv6 packet max size (RFC 8415 §7.3) */
#define DHCPV6_MAX_MSG_SIZE 1024

/* Initialize DHCPv6 client state */
void dhcpv6_init(void);

/* Start a DHCPv6 Solicit → Advertise → Request → Reply handshake.
 * Requires an IPv6 link-local address to be configured.
 * Returns 0 on success (lease acquired), negative on failure. */
int  dhcpv6_handshake(void);

/* Returns 1 if a DHCPv6 lease is held, 0 otherwise */
int  dhcpv6_has_lease(void);

/* Get the assigned IPv6 address (in network byte order) */
void dhcpv6_get_assigned_addr(struct in6_addr *out);

/* Get the DHCPv6 server DUID bytes and length */
void dhcpv6_get_server_id(const uint8_t **data, int *len);

/* Get DNS server learned via DHCPv6 */
void dhcpv6_get_dns_server(struct in6_addr *out);
#endif
