#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "dhcp.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "timer.h"
#include "e1000.h"
#include "virtio_net.h"
#include "errno.h"

/*
 * DHCPv6 client implementation (RFC 8415).
 *
 * Performs a 4-way DHCPv6 handshake over UDP/IPv6:
 *   1. SOLICIT (multicast to All_DHCP_Servers FF02::1:2 on UDP 547)
 *   2. Receive ADVERTISE from server (unicast)
 *   3. Send REQUEST (unicast to server)
 *   4. Receive REPLY with IPv6 address assignment
 *
 * Uses DUID-LL (type 3) based on the MAC address for client identification.
 * Requires a configured IPv6 link-local address before starting.
 *
 * References:
 *   - RFC 8415: Dynamic Host Configuration Protocol for IPv6 (DHCPv6)
 *   - RFC 3646: DNS Configuration options for DHCPv6
 */

/* ── DHCPv6 packet structures ─────────────────────────────────────── */

/* DHCPv6 message header (RFC 8415 §7.3):
 *   msg-type   (1 byte)
 *   transaction-id (3 bytes)
 *   options... (variable)
 */
struct dhcpv6_header {
	uint8_t  msg_type;
	uint8_t  transaction_id[3];
	/* options follow */
} __attribute__((packed));

/* DHCPv6 option TLV header (RFC 8415 §7.4):
 *   option-code   (2 bytes, network byte order)
 *   option-len    (2 bytes, network byte order)
 *   option-data   (variable)
 */
struct dhcpv6_option {
	uint16_t code;
	uint16_t len;
	/* data follows */
} __attribute__((packed));

/* DUID-LL format (RFC 8415 §11.3):
 *   duid-type    (2 bytes, network byte order) = 3
 *   hw-type      (2 bytes, network byte order) = 1 (Ethernet)
 *   link-layer-addr (variable, 6 bytes for Ethernet)
 */
struct duid_ll {
	uint16_t duid_type;
	uint16_t hw_type;
	uint8_t  mac[6];
} __attribute__((packed));

/* IA_NA option (RFC 8415 §21.4):
 *   option-code  (2 bytes) = OPTION_IA_NA (3)
 *   option-len   (2 bytes)
 *   IAID         (4 bytes)
 *   T1           (4 bytes)
 *   T2           (4 bytes)
 *   options (IA_ADDR, ...)
 */
struct ia_na_option {
	struct dhcpv6_option hdr;
	uint32_t iaid;
	uint32_t t1;
	uint32_t t2;
	/* sub-options follow */
} __attribute__((packed));

/* IA_ADDR sub-option (RFC 8415 §21.6):
 *   option-code  (2 bytes) = OPTION_IA_ADDR (5)
 *   option-len   (2 bytes) = 24
 *   IPv6 address (16 bytes)
 *   preferred-lifetime (4 bytes)
 *   valid-lifetime    (4 bytes)
 */
struct ia_addr_option {
	struct dhcpv6_option hdr;
	struct in6_addr addr;
	uint32_t preferred_lifetime;
	uint32_t valid_lifetime;
} __attribute__((packed));

/* ── State ────────────────────────────────────────────────────────── */

/* Our DUID (built once from MAC) */
static uint8_t dhcpv6_duid[DHCPV6_DUID_LL_LEN];
static int     dhcpv6_duid_len = 0;

/* Transaction ID for the current exchange */
static uint32_t dhcpv6_xid = 0;  /* only lower 24 bits used */

/* Assigned IPv6 address (network byte order) */
static struct in6_addr dhcpv6_assigned_addr;
static int             dhcpv6_has_lease_v6 = 0;

/* Server DUID (saved from ADVERTISE) */
static uint8_t  dhcpv6_server_duid[128];
static int      dhcpv6_server_duid_len = 0;

/* DNS server learned via DHCPv6 OPTION_DNS_SERVERS (RFC 3646) */
static struct in6_addr dhcpv6_dns_server;
static int             dhcpv6_has_dns = 0;

/* IAID (Identity Association Identifier) — we use a fixed value */
#define DHCPV6_IAID 0x00000001U

/* All_DHCP_Servers multicast address (FF02::1:2) */
static const struct in6_addr dhcpv6_all_servers = IPV6_ADDR_ALL_DHCP_SERVERS;

/* Solicit retry constants */
#define DHCPV6_SOLICIT_RETRIES  3
#define DHCPV6_SOLICIT_TIMEOUT  300  /* 3 seconds per retry in timer ticks (100 Hz) */
#define DHCPV6_RESPONSE_TIMEOUT 500  /* 5 second total timeout */

/* Polling helpers forward declarations */
static int dhcpv6_poll_network(uint8_t *buf, int max_len, uint16_t *out_len);

/* ── DUID construction ────────────────────────────────────────────── */

/*
 * Build a DUID-LL (type 3) from the MAC address.
 * The DUID is stored in dhcpv6_duid[] and is reused for all transactions.
 */
static void dhcpv6_build_duid(void)
{
	struct duid_ll *duid = (struct duid_ll *)dhcpv6_duid;

	duid->duid_type = htons(DUID_LL);       /* DUID-LL */
	duid->hw_type   = htons(1);             /* Ethernet (RFC 826) */
	memcpy(duid->mac, net_our_mac, 6);

	dhcpv6_duid_len = DHCPV6_DUID_LL_LEN;
}

/* ── Low-level IPv6/UDP send ──────────────────────────────────────── */

/*
 * Build and send a UDP datagram over IPv6 to a given destination.
 * Constructs the UDP header around the given payload, computes the
 * UDP checksum (with IPv6 pseudo-header), and sends via send_ipv6().
 */
static int dhcpv6_send_udp(const struct in6_addr *dst, uint16_t dst_port,
                            const void *data, uint16_t data_len)
{
	uint8_t buf[DHCPV6_MAX_MSG_SIZE + 64];
	struct udp_header *udp;
	uint16_t udp_len;
	int payload_needed;

	payload_needed = (int)sizeof(struct udp_header) + (int)data_len;
	if (payload_needed > (int)(sizeof(buf) - sizeof(struct ipv6_header))) {
		kprintf("[dhcpv6] send_udp: packet too large (%d)\n", payload_needed);
		return -EMSGSIZE;
	}

	udp = (struct udp_header *)buf;
	udp_len = sizeof(struct udp_header) + data_len;

	udp->src_port = htons(DHCPV6_CLIENT_PORT);
	udp->dst_port = htons(dst_port);
	udp->length   = htons(udp_len);
	udp->checksum = 0; /* computed by send_ipv6 via IPv6 pseudo-header */

	if (data_len > 0)
		memcpy(buf + sizeof(struct udp_header), data, data_len);

	send_ipv6(dst, IPV6_NEXTHDR_UDP, buf, udp_len);
	return 0;
}

/*
 * Send a DHCPv6 message to the All_DHCP_Servers multicast address
 * (FF02::1:2) on UDP port 547.
 */
static int dhcpv6_send_multicast(const void *msg, uint16_t msg_len)
{
	return dhcpv6_send_udp(&dhcpv6_all_servers, DHCPV6_SERVER_PORT, msg, msg_len);
}

/*
 * Send a DHCPv6 message as unicast to a server (identified by its
 * link-local address, which is the source of the ADVERTISE).
 */
static int dhcpv6_send_unicast(const struct in6_addr *server,
                                const void *msg, uint16_t msg_len)
{
	return dhcpv6_send_udp(server, DHCPV6_SERVER_PORT, msg, msg_len);
}

/* ── DHCPv6 message building helpers ──────────────────────────────── */

/* Write a DHCPv6 option TLV header + data into buf at *offset.
 * Returns the new offset after the written data.
 */
static uint8_t *dhcpv6_write_option(uint8_t *ptr, uint16_t code,
                                     const void *data, uint16_t len)
{
	struct dhcpv6_option *opt = (struct dhcpv6_option *)ptr;
	opt->code = htons(code);
	opt->len  = htons(len);
	if (len > 0 && data)
		memcpy(ptr + sizeof(struct dhcpv6_option), data, len);
	return ptr + sizeof(struct dhcpv6_option) + len;
}

/* Write the CLIENTID option containing our DUID. */
static uint8_t *dhcpv6_write_clientid(uint8_t *ptr)
{
	return dhcpv6_write_option(ptr, DHCPV6_OPTION_CLIENTID,
	                           dhcpv6_duid, (uint16_t)dhcpv6_duid_len);
}

/* Write the SERVERID option containing the server's DUID. */
static uint8_t *dhcpv6_write_serverid(uint8_t *ptr)
{
	return dhcpv6_write_option(ptr, DHCPV6_OPTION_SERVERID,
	                           dhcpv6_server_duid,
	                           (uint16_t)dhcpv6_server_duid_len);
}

/* Write an ELAPSED_TIME option (RFC 8415 §21.2).
 * elapsed is in centiseconds (1/100 sec). */
static uint8_t *dhcpv6_write_elapsed_time(uint8_t *ptr, uint16_t elapsed_cs)
{
	uint16_t net_elapsed = htons(elapsed_cs);
	return dhcpv6_write_option(ptr, DHCPV6_OPTION_ELAPSED_TIME,
	                           &net_elapsed, sizeof(net_elapsed));
}

/* Write an IA_NA option with one IA_ADDR sub-option (for the requested address).
 * If addr is all-zeros, the server selects the address. */
static uint8_t *dhcpv6_write_iana(uint8_t *ptr,
                                   const struct in6_addr *addr,
                                   uint32_t preferred_lifetime,
                                   uint32_t valid_lifetime)
{
	struct ia_na_option *iana;
	struct ia_addr_option *iaaddr;
	uint16_t iana_len;

	/* IA_NA itself: hdr(4) + IAID(4) + T1(4) + T2(4) = 16 bytes header
	 * Then the IA_ADDR sub-option:
	 *   hdr(4) + addr(16) + pref_lft(4) + valid_lft(4) = 28 bytes
	 * Total: 44 bytes
	 *
	 * But wait — the dhcpv6_option header is size 4 (code(2)+len(2)).
	 * Let's recalculate:
	 * IA_NA option: hdr(4) + iaid(4) + t1(4) + t2(4) = 16 bytes
	 * IA_ADDR sub-option: hdr(4) + addr(16) + pref(4) + valid(4) = 28 bytes
	 * Total option bytes (the "len" field of IA_NA): iaid+t1+t2 + IA_ADDR_size = 12 + 28 = 40
	 */

	/* Write IA_NA: code + len + iaid + t1 + t2 */
	iana = (struct ia_na_option *)ptr;
	iana->hdr.code = htons(DHCPV6_OPTION_IA_NA);
	/* len = iaid(4) + t1(4) + t2(4) + IA_ADDR sub-option total */
	iana_len = DHCPV6_IA_NA_HDR_LEN +  /* IAID + T1 + T2 */
	           sizeof(struct dhcpv6_option) + DHCPV6_IA_ADDR_LEN; /* IA_ADDR sub-option */
	iana->hdr.len = htons(iana_len);
	iana->iaid = htonl(DHCPV6_IAID);
	iana->t1   = htonl(0);  /* 0 = server determines T1 */
	iana->t2   = htonl(0);  /* 0 = server determines T2 */

	/* Write IA_ADDR sub-option inside the IA_NA */
	iaaddr = (struct ia_addr_option *)(ptr + sizeof(struct ia_na_option));
	iaaddr->hdr.code = htons(DHCPV6_OPTION_IA_ADDR);
	iaaddr->hdr.len  = htons(DHCPV6_IA_ADDR_LEN);
	if (addr)
		memcpy(&iaaddr->addr, addr, sizeof(struct in6_addr));
	else
		memset(&iaaddr->addr, 0, sizeof(struct in6_addr));
	iaaddr->preferred_lifetime = htonl(preferred_lifetime);
	iaaddr->valid_lifetime     = htonl(valid_lifetime);

	return ptr + sizeof(struct ia_na_option) + sizeof(struct ia_addr_option);
}

/* Write the ORO (Option Request Option) with requested option codes. */
static uint8_t *dhcpv6_write_oro(uint8_t *ptr)
{
	/* Request DNS servers (23) and domain list (24) */
	uint16_t codes[2];
	codes[0] = htons(23); /* OPTION_DNS_SERVERS (RFC 3646) */
	codes[1] = htons(24); /* OPTION_DOMAIN_LIST */
	return dhcpv6_write_option(ptr, DHCPV6_OPTION_ORO,
	                           codes, sizeof(codes));
}

/* ── Build SOLICIT message ─────────────────────────────────────────── */

/*
 * Build a DHCPv6 SOLICIT message in the provided buffer.
 * Returns the total message length, or negative on error.
 */
static int dhcpv6_build_solicit(uint8_t *buf, int buf_len)
{
	uint8_t *ptr;
	struct dhcpv6_header *hdr;
	int msg_len;

	if (buf_len < DHCPV6_MAX_MSG_SIZE)
		return -ENOSPC;

	hdr = (struct dhcpv6_header *)buf;
	hdr->msg_type = DHCPV6_SOLICIT;
	hdr->transaction_id[0] = (uint8_t)(dhcpv6_xid >> 16);
	hdr->transaction_id[1] = (uint8_t)(dhcpv6_xid >> 8);
	hdr->transaction_id[2] = (uint8_t)(dhcpv6_xid);

	ptr = buf + sizeof(struct dhcpv6_header);

	/* Client Identifier (mandatory) */
	ptr = dhcpv6_write_clientid(ptr);

	/* IA_NA — ask for a non-temporary address */
	ptr = dhcpv6_write_iana(ptr, NULL, 0, 0);

	/* Option Request Option (ORO) */
	ptr = dhcpv6_write_oro(ptr);

	/* Elapsed Time */
	ptr = dhcpv6_write_elapsed_time(ptr, 0);

	msg_len = (int)(ptr - buf);
	return msg_len;
}

/* ── Build REQUEST message ─────────────────────────────────────────── */

/*
 * Build a DHCPv6 REQUEST message after receiving an ADVERTISE.
 * Uses the assigned address (if any) and the server DUID.
 */
static int dhcpv6_build_request(uint8_t *buf, int buf_len)
{
	uint8_t *ptr;
	struct dhcpv6_header *hdr;
	int msg_len;

	if (buf_len < DHCPV6_MAX_MSG_SIZE)
		return -ENOSPC;

	hdr = (struct dhcpv6_header *)buf;
	hdr->msg_type = DHCPV6_REQUEST;
	hdr->transaction_id[0] = (uint8_t)(dhcpv6_xid >> 16);
	hdr->transaction_id[1] = (uint8_t)(dhcpv6_xid >> 8);
	hdr->transaction_id[2] = (uint8_t)(dhcpv6_xid);

	ptr = buf + sizeof(struct dhcpv6_header);

	/* Client Identifier */
	ptr = dhcpv6_write_clientid(ptr);

	/* Server Identifier (copied from ADVERTISE) */
	if (dhcpv6_server_duid_len > 0)
		ptr = dhcpv6_write_serverid(ptr);

	/* IA_NA with our preferred address (the one offered) */
	ptr = dhcpv6_write_iana(ptr, &dhcpv6_assigned_addr, 0, 0);

	/* Option Request Option */
	ptr = dhcpv6_write_oro(ptr);

	/* Elapsed Time */
	ptr = dhcpv6_write_elapsed_time(ptr, 0);

	msg_len = (int)(ptr - buf);
	return msg_len;
}

/* ── Parse and handle DHCPv6 replies ───────────────────────────────── */

/*
 * Parse the DHCPv6 options in the response and extract relevant fields.
 * Returns the DHCPv6 message type on success, negative on unrecognised.
 *
 * For ADVERTISE (2): saves server DUID, offered address, DNS servers.
 * For REPLY (7): validates status, saves assigned address, DNS servers.
 */
static int dhcpv6_parse_response(const uint8_t *data, uint16_t len)
{
	const struct dhcpv6_header *hdr;
	const uint8_t *ptr;
	const uint8_t *end;
	uint8_t msg_type;
	uint32_t rx_xid;
	int has_serverid = 0;
	int has_iana = 0;

	if (len < sizeof(struct dhcpv6_header))
		return -EINVAL;

	hdr = (const struct dhcpv6_header *)data;
	msg_type = hdr->msg_type;

	/* Verify transaction ID matches */
	rx_xid = ((uint32_t)hdr->transaction_id[0] << 16) |
	         ((uint32_t)hdr->transaction_id[1] << 8)  |
	         hdr->transaction_id[2];
	if (rx_xid != dhcpv6_xid) {
		kprintf("[dhcpv6] xid mismatch: local=0x%06x, rx=0x%06x\n",
		        (unsigned)dhcpv6_xid, (unsigned)rx_xid);
		return -EAGAIN;  /* not for us, try again */
	}

	/* Only handle ADVERTISE and REPLY */
	if (msg_type != DHCPV6_ADVERTISE && msg_type != DHCPV6_REPLY) {
		kprintf("[dhcpv6] unexpected msg type %u\n", msg_type);
		return -EBADMSG;
	}

	ptr = data + sizeof(struct dhcpv6_header);
	end = data + len;

	while (ptr + 4 <= end) {
		const struct dhcpv6_option *opt = (const struct dhcpv6_option *)ptr;
		uint16_t opt_code = ntohs(opt->code);
		uint16_t opt_len  = ntohs(opt->len);
		const uint8_t *opt_data = ptr + sizeof(struct dhcpv6_option);

		if (opt_len > 0 && opt_data + opt_len > end) {
			kprintf("[dhcpv6] option %u truncated\n", opt_code);
			break;
		}

		switch (opt_code) {
		case DHCPV6_OPTION_CLIENTID:
			/* Echo of our client ID — skip */
			break;

		case DHCPV6_OPTION_SERVERID:
			/* Save server DUID for REQUEST */
			if (opt_len > sizeof(dhcpv6_server_duid)) {
				kprintf("[dhcpv6] server DUID too large (%u)\n", opt_len);
				break;
			}
			memcpy(dhcpv6_server_duid, opt_data, opt_len);
			dhcpv6_server_duid_len = opt_len;
			has_serverid = 1;
			break;

		case DHCPV6_OPTION_IA_NA: {
			/* Parse IA_NA for IA_ADDR sub-option */
			const uint8_t *ia_ptr = opt_data;
			const uint8_t *ia_end = opt_data + opt_len;

			if (opt_len < 12)
				break; /* IAID(4)+T1(4)+T2(4) minimum */

			ia_ptr += 12; /* skip IAID, T1, T2 */

			while (ia_ptr + 4 <= ia_end) {
				const struct dhcpv6_option *ia_opt;
				const uint8_t *ia_opt_data;

				ia_opt = (const struct dhcpv6_option *)ia_ptr;
				uint16_t ia_opt_code = ntohs(ia_opt->code);
				uint16_t ia_opt_len  = ntohs(ia_opt->len);
				ia_opt_data = ia_ptr + sizeof(struct dhcpv6_option);

				if (ia_opt_len > 0 && ia_opt_data + ia_opt_len > ia_end)
					break;

				if (ia_opt_code == DHCPV6_OPTION_IA_ADDR && ia_opt_len >= 24) {
					/* IPv6 address (16) + pref_lft(4) + valid_lft(4) */
					memcpy(&dhcpv6_assigned_addr, ia_opt_data,
					       sizeof(struct in6_addr));
					has_iana = 1;
				}

				ia_ptr = ia_opt_data + ia_opt_len;
			}
			break;
		}

		case DHCPV6_OPTION_STATUS_CODE:
			/* Status Code: status(2 bytes) + message (optional) */
			if (opt_len >= 2) {
				uint16_t status = (uint16_t)opt_data[0] << 8 | opt_data[1];
				if (status != DHCPV6_STATUS_SUCCESS) {
					kprintf("[dhcpv6] server returned status %u\n", status);
					return -ENODATA;
				}
			}
			break;

		case 23: /* OPTION_DNS_SERVERS (RFC 3646) */
			/* Data is a list of IPv6 addresses (each 16 bytes) */
			if (opt_len >= (int)sizeof(struct in6_addr)) {
				memcpy(&dhcpv6_dns_server, opt_data,
				       sizeof(struct in6_addr));
				dhcpv6_has_dns = 1;
			}
			break;

		default:
			/* Unknown option — skip */
			break;
		}

		ptr = opt_data + opt_len;
	}

	/* For ADVERTISE, we must have a server ID to proceed.
	 * For REPLY, we need the IA_NA assignment. */
	if (msg_type == DHCPV6_ADVERTISE && !has_serverid) {
		kprintf("[dhcpv6] ADVERTISE missing server ID\n");
		return -EBADMSG;
	}

	return (int)msg_type;
}

/* ── Poll network for incoming DHCPv6 packets ──────────────────────── */

/*
 * Poll all available network interfaces for incoming UDP/IPv6 packets
 * directed to the DHCPv6 client port (546).
 *
 * Returns the DHCPv6 message type on successful parse,
 * -EAGAIN if no relevant packet found, or another negative on error.
 */
static int dhcpv6_poll_for_reply(void)
{
	uint8_t pkt[2048];
	int n;

	/* Poll e1000 */
	if (e1000_is_present()) {
		n = e1000_receive(pkt, sizeof(pkt));
		if (n > 0) {
			struct eth_header *eth = (struct eth_header *)pkt;
			if (ntohs(eth->type) == ETH_TYPE_IPV6 &&
			    n >= (int)(sizeof(struct eth_header) + sizeof(struct ipv6_header))) {
				struct ipv6_header *ip6 = (struct ipv6_header *)
					(pkt + sizeof(struct eth_header));
				uint16_t payload_len = ntohs(ip6->payload_length);
				int ip6_hdr_off = (int)sizeof(struct eth_header);

				if (payload_len >= sizeof(struct udp_header) &&
				    ip6->next_header == IPV6_NEXTHDR_UDP &&
				    (int)(ip6_hdr_off + sizeof(struct ipv6_header) + payload_len) <= n) {
					struct udp_header *udp = (struct udp_header *)
						(pkt + ip6_hdr_off + sizeof(struct ipv6_header));
					if (ntohs(udp->dst_port) == DHCPV6_CLIENT_PORT) {
						uint16_t udp_len = ntohs(udp->length);
						int data_off = ip6_hdr_off + (int)sizeof(struct ipv6_header)
						              + (int)sizeof(struct udp_header);
						int data_len = (int)(udp_len - sizeof(struct udp_header));
						if (data_off + data_len <= n && data_len > 0) {
							return dhcpv6_parse_response(
								pkt + data_off, (uint16_t)data_len);
						}
					}
				}
			}
		}
	}

	/* Poll virtio_net */
	if (virtio_net_present()) {
		n = virtio_net_receive(pkt, sizeof(pkt));
		if (n > 0) {
			struct eth_header *eth = (struct eth_header *)pkt;
			if (ntohs(eth->type) == ETH_TYPE_IPV6 &&
			    n >= (int)(sizeof(struct eth_header) + sizeof(struct ipv6_header))) {
				struct ipv6_header *ip6 = (struct ipv6_header *)
					(pkt + sizeof(struct eth_header));
				uint16_t payload_len = ntohs(ip6->payload_length);
				int ip6_hdr_off = (int)sizeof(struct eth_header);

				if (payload_len >= sizeof(struct udp_header) &&
				    ip6->next_header == IPV6_NEXTHDR_UDP &&
				    (int)(ip6_hdr_off + sizeof(struct ipv6_header) + payload_len) <= n) {
					struct udp_header *udp = (struct udp_header *)
						(pkt + ip6_hdr_off + sizeof(struct ipv6_header));
					if (ntohs(udp->dst_port) == DHCPV6_CLIENT_PORT) {
						uint16_t udp_len = ntohs(udp->length);
						int data_off = ip6_hdr_off + (int)sizeof(struct ipv6_header)
						              + (int)sizeof(struct udp_header);
						int data_len = (int)(udp_len - sizeof(struct udp_header));
						if (data_off + data_len <= n && data_len > 0) {
							return dhcpv6_parse_response(
								pkt + data_off, (uint16_t)data_len);
						}
					}
				}
			}
		}
	}

	return -EAGAIN;
}

/* ── Public API ────────────────────────────────────────────────────── */

void dhcpv6_init(void)
{
	dhcpv6_has_lease_v6 = 0;
	dhcpv6_xid = 0;
	dhcpv6_server_duid_len = 0;
	dhcpv6_has_dns = 0;

	/* Build DUID from MAC */
	dhcpv6_build_duid();

	kprintf("[OK] DHCPv6 client initialized (DUID-LL, %d bytes)\n",
	        dhcpv6_duid_len);
}

int dhcpv6_handshake(void)
{
	uint8_t msg_buf[DHCPV6_MAX_MSG_SIZE];
	int msg_len;
	int retry;
	int result;

	if (!net_ipv6_ll_ready) {
		kprintf("[dhcpv6] cannot start: no IPv6 link-local address\n");
		return -ENETDOWN;
	}

	/* Generate a 24-bit random transaction ID */
	dhcpv6_xid = (uint32_t)(timer_get_ticks() & 0x00FFFFFFU);
	/* Mix in some MAC-derived bits for randomness */
	dhcpv6_xid ^= (uint32_t)net_our_mac[4] << 16;
	dhcpv6_xid ^= (uint32_t)net_our_mac[5] << 8;
	dhcpv6_xid ^= (uint32_t)net_our_mac[3];
	dhcpv6_xid &= 0x00FFFFFFU;

	kprintf("[dhcpv6] Starting Solicit (xid=0x%06x)\n",
	        (unsigned)dhcpv6_xid);

	/* ── SOLICIT phase ─────────────────────────────────────────── */
	msg_len = dhcpv6_build_solicit(msg_buf, sizeof(msg_buf));
	if (msg_len < 0)
		return msg_len;

	dhcpv6_send_multicast(msg_buf, (uint16_t)msg_len);

	/* Wait for ADVERTISE with retries */
	retry = 0;
	while (retry < DHCPV6_SOLICIT_RETRIES) {
		uint64_t start = timer_get_ticks();
		uint64_t elapsed;

		while (1) {
			elapsed = timer_get_ticks() - start;
			if (elapsed >= DHCPV6_SOLICIT_TIMEOUT)
				break;

			result = dhcpv6_poll_for_reply();
			if (result == DHCPV6_ADVERTISE) {
				/* Got an ADVERTISE! */
				kprintf("[dhcpv6] Received ADVERTISE from server\n");
				goto got_advertise;
			}
			if (result != -EAGAIN) {
				kprintf("[dhcpv6] poll error: %d\n", result);
			}
		}

		/* Resend SOLICIT */
		retry++;
		if (retry < DHCPV6_SOLICIT_RETRIES) {
			kprintf("[dhcpv6] Solicit timeout, resending (%d/%d)\n",
			        retry, DHCPV6_SOLICIT_RETRIES);
			dhcpv6_send_multicast(msg_buf, (uint16_t)msg_len);
		}
	}

	kprintf("[dhcpv6] No ADVERTISE received after %d retries\n",
	        DHCPV6_SOLICIT_RETRIES);
	return -ETIMEDOUT;

got_advertise:
	/* ── REQUEST phase ─────────────────────────────────────────── */
	msg_len = dhcpv6_build_request(msg_buf, sizeof(msg_buf));
	if (msg_len < 0)
		return msg_len;

	/* Send REQUEST to the All_DHCP_Servers multicast address
	 * (RFC 8415 §18.2.2 allows REQUEST to be multicast). */
	dhcpv6_send_unicast(&dhcpv6_all_servers, msg_buf, (uint16_t)msg_len);

	kprintf("[dhcpv6] Sent REQUEST, waiting for REPLY...\n");

	/* Wait for REPLY */
	{
		uint64_t start = timer_get_ticks();
		int reply_retries = 0;

		while (reply_retries < 3) {
			uint64_t now = timer_get_ticks();
			uint64_t elapsed = now - start;

			if (elapsed >= DHCPV6_RESPONSE_TIMEOUT)
				break;

			result = dhcpv6_poll_for_reply();
			if (result == DHCPV6_REPLY) {
				kprintf("[dhcpv6] Received REPLY — lease acquired!\n");

				/* Log the assigned address */
				kprintf("[dhcpv6] Assigned address: %02x%02x:%02x%02x:%02x%02x:%02x%02x"
				        ":%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
				        dhcpv6_assigned_addr.s6_addr[0],
				        dhcpv6_assigned_addr.s6_addr[1],
				        dhcpv6_assigned_addr.s6_addr[2],
				        dhcpv6_assigned_addr.s6_addr[3],
				        dhcpv6_assigned_addr.s6_addr[4],
				        dhcpv6_assigned_addr.s6_addr[5],
				        dhcpv6_assigned_addr.s6_addr[6],
				        dhcpv6_assigned_addr.s6_addr[7],
				        dhcpv6_assigned_addr.s6_addr[8],
				        dhcpv6_assigned_addr.s6_addr[9],
				        dhcpv6_assigned_addr.s6_addr[10],
				        dhcpv6_assigned_addr.s6_addr[11],
				        dhcpv6_assigned_addr.s6_addr[12],
				        dhcpv6_assigned_addr.s6_addr[13],
				        dhcpv6_assigned_addr.s6_addr[14],
				        dhcpv6_assigned_addr.s6_addr[15]);

				if (dhcpv6_has_dns) {
					kprintf("[dhcpv6] DNS server received via DHCPv6\n");
				}

				/* Add the assigned address to the kernel's address table */
				ipv6_addr_add(&dhcpv6_assigned_addr, 64,
				               IPV6_ADDR_STATE_PREFERRED, 0xFFFFFFFFU,
				               0xFFFFFFFFU, IPV6_ADDR_F_AUTOCONF);

				dhcpv6_has_lease_v6 = 1;
				return 0;
			}
			if (result != -EAGAIN) {
				kprintf("[dhcpv6] poll error during REQUEST phase: %d\n", result);
			}

			/* Resend REQUEST every ~1 second */
			if (elapsed > (uint64_t)(reply_retries + 1) * 100) {
				reply_retries++;
				kprintf("[dhcpv6] Resending REQUEST (attempt %d/3)\n", reply_retries);
				dhcpv6_send_unicast(&dhcpv6_all_servers, msg_buf, (uint16_t)msg_len);
			}
		}
	}

	kprintf("[dhcpv6] REPLY not received — handshake failed\n");
	return -ETIMEDOUT;
}

int dhcpv6_has_lease(void)
{
	return dhcpv6_has_lease_v6;
}

void dhcpv6_get_assigned_addr(struct in6_addr *out)
{
	if (out)
		memcpy(out, &dhcpv6_assigned_addr, sizeof(struct in6_addr));
}

void dhcpv6_get_server_id(const uint8_t **data, int *len)
{
	if (data)
		*data = dhcpv6_server_duid;
	if (len)
		*len = dhcpv6_server_duid_len;
}

void dhcpv6_get_dns_server(struct in6_addr *out)
{
	if (out) {
		if (dhcpv6_has_dns)
			memcpy(out, &dhcpv6_dns_server, sizeof(struct in6_addr));
		else
			memset(out, 0, sizeof(struct in6_addr));
	}
}
