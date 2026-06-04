/* conntrack_helpers.c — Conntrack ALG (Application Layer Gateway) helpers
 *
 * Implements the helper infrastructure for protocol ALGs plus the FTP
 * control-channel helper.  Currently supported:
 *   - FTP helper: parses PORT (active mode) commands on TCP/21 and creates
 *     expected connection entries for the data channel.
 *
 * Expected connection entries allow conntrack to mark data-channel packets
 * as NF_CONN_RELATED to the control connection, which is essential for
 * proper NAT tracking of multi-channel protocols like FTP.
 *
 * Reference: Linux netfilter conntrack helpers (nf_conntrack_ftp),
 *            RFC 959 (File Transfer Protocol).
 */

#define KERNEL_INTERNAL
#include "conntrack_helper.h"
#include "netfilter.h"
#include "net.h"
#include "spinlock.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "heap.h"

/* ══════════════════════════════════════════════════════════════════
 *                  Static State — Expected Connections
 * ══════════════════════════════════════════════════════════════════ */

static struct nf_ct_expect nf_exp_table[NF_CT_EXPECT_MAX];
static int                 nf_exp_count;
static spinlock_t          nf_exp_lock;
static int                 nf_exp_initialized;

/* ══════════════════════════════════════════════════════════════════
 *                  Static State — Registered Helpers
 * ══════════════════════════════════════════════════════════════════ */

#define NF_HELPER_MAX  8
static struct nf_helper nf_helpers[NF_HELPER_MAX];
static int              nf_helper_count;

/* ══════════════════════════════════════════════════════════════════
 *              Expected Connection Table Management
 * ══════════════════════════════════════════════════════════════════ */

int nf_ct_expect_related(struct nf_conn *master,
                         uint32_t src_ip, uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         uint8_t protocol)
{
    (void)master;

    if (!nf_exp_initialized) return -1;

    spinlock_acquire(&nf_exp_lock);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < NF_CT_EXPECT_MAX; i++) {
        if (!nf_exp_table[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        spinlock_release(&nf_exp_lock);
        kprintf("[CT-HELPER] Expected connection table full!\n");
        return -1;
    }

    nf_exp_table[slot].src_ip   = src_ip;
    nf_exp_table[slot].dst_ip   = dst_ip;
    nf_exp_table[slot].src_port = src_port;
    nf_exp_table[slot].dst_port = dst_port;
    nf_exp_table[slot].protocol = protocol;
    nf_exp_table[slot].used     = 1;
    nf_exp_table[slot].expiry   = timer_get_ticks() + 300;  /* 30 second timeout */
    nf_exp_count++;

    spinlock_release(&nf_exp_lock);
    return 0;
}

struct nf_ct_expect *nf_ct_expect_lookup(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port, uint16_t dst_port,
                                          uint8_t protocol)
{
    if (!nf_exp_initialized) return NULL;

    spinlock_acquire(&nf_exp_lock);

    /* Match forward: expected (src->dst) == packet (src->dst) */
    for (int i = 0; i < NF_CT_EXPECT_MAX; i++) {
        struct nf_ct_expect *e = &nf_exp_table[i];
        if (!e->used) continue;

        if (e->src_ip   == src_ip   && e->dst_ip   == dst_ip &&
            e->src_port == src_port && e->dst_port == dst_port &&
            e->protocol == protocol) {
            spinlock_release(&nf_exp_lock);
            return e;
        }

        /* Match reverse: expected (dst->src) == packet (src->dst) */
        if (e->dst_ip   == src_ip   && e->src_ip   == dst_ip &&
            e->dst_port == src_port && e->src_port == dst_port &&
            e->protocol == protocol) {
            spinlock_release(&nf_exp_lock);
            return e;
        }
    }

    spinlock_release(&nf_exp_lock);
    return NULL;
}

void nf_ct_expect_clear(struct nf_conn *master)
{
    (void)master;
    /* For now, clear all expected entries owned by this helper.
     * A full implementation would track owner connection. */
    spinlock_acquire(&nf_exp_lock);
    for (int i = 0; i < NF_CT_EXPECT_MAX; i++) {
        if (nf_exp_table[i].used) {
            nf_exp_table[i].used = 0;
            nf_exp_count--;
        }
    }
    spinlock_release(&nf_exp_lock);
}

void nf_ct_expect_purge(void)
{
    if (!nf_exp_initialized) return;

    uint64_t now = timer_get_ticks();
    spinlock_acquire(&nf_exp_lock);

    for (int i = 0; i < NF_CT_EXPECT_MAX; i++) {
        if (nf_exp_table[i].used && now > nf_exp_table[i].expiry) {
            nf_exp_table[i].used = 0;
            nf_exp_count--;
        }
    }

    spinlock_release(&nf_exp_lock);
}

/* ══════════════════════════════════════════════════════════════════
 *               Helper Registration Management
 * ══════════════════════════════════════════════════════════════════ */

int nf_helper_register(uint8_t protocol, uint16_t dst_port,
                       nf_helper_hook_t fn, const char *name)
{
    if (!fn || !name) return -1;
    if (nf_helper_count >= NF_HELPER_MAX) {
        kprintf("[CT-HELPER] Helper table full (max %d)\n", NF_HELPER_MAX);
        return -1;
    }

    struct nf_helper *h = &nf_helpers[nf_helper_count];
    h->protocol = protocol;
    h->dst_port = dst_port;
    h->hook_fn  = fn;
    h->used     = 1;
    strncpy(h->name, name, sizeof(h->name) - 1);
    h->name[sizeof(h->name) - 1] = '\0';
    nf_helper_count++;

    kprintf("[CT-HELPER] Registered helper '%s' for proto=%u port=%u\n",
            name, (unsigned)protocol, (unsigned)dst_port);
    return 0;
}

void nf_helper_unregister(nf_helper_hook_t fn)
{
    if (!fn) return;
    for (int i = 0; i < NF_HELPER_MAX; i++) {
        if (nf_helpers[i].used && nf_helpers[i].hook_fn == fn) {
            nf_helpers[i].used = 0;
            nf_helper_count--;

            /* Log unregistration before compacting */
            kprintf("[CT-HELPER] Unregistered helper '%s'\n", nf_helpers[i].name);

            /* Compact the array */
            for (int j = i; j < NF_HELPER_MAX - 1; j++)
                nf_helpers[j] = nf_helpers[j + 1];
            nf_helpers[NF_HELPER_MAX - 1].used = 0;
            return;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 *              Netfilter Hook — Dispatcher to Helpers
 * ══════════════════════════════════════════════════════════════════
 *
 * This hook is registered at NF_INET_PRE_ROUTING and NF_INET_LOCAL_OUT.
 * For each packet, it checks if a helper has registered for its
 * (protocol, dst_port) tuple and, if so, calls the helper's hook_fn.
 *
 * The packet buffer passed to hooks contains:
 *   PRE_ROUTING:  [eth header | ip header | tcp header | payload...]
 *   LOCAL_OUT:    [ip header | tcp header | payload...]
 *
 * We always parse from the Ethernet header when available; the LOCAL_OUT
 * buffer doesn't have one, so we detect and handle both cases.
 */

static int helper_dispatch_hook(void *skb, int hook)
{
    (void)hook;

    if (!skb || nf_helper_count == 0)
        return NF_ACCEPT;

    uint8_t *buf = (uint8_t *)skb;

    /* ── Determine buffer layout ─────────────────────────────────
     * For LOCAL_OUT, the buffer starts at the IP header.
     * For PRE_ROUTING, it starts at the Ethernet header.
     * Heuristic: if the first byte looks like an Ethernet header
     * (MAC addresses are never 0x45 which is IPv4 version+IHL),
     * assume it's a full Ethernet frame. */
    int eth_present = (buf[0] != 0x45);
    int ip_off = eth_present ? (int)sizeof(struct eth_header) : 0;

    /* Parse IP header */
    if (ip_off + (int)sizeof(struct ip_header) < 0) return NF_ACCEPT;
    struct ip_header *ip = (struct ip_header *)(buf + ip_off);

    /* Must be IPv4 */
    if ((ip->version_ihl >> 4) != 4)
        return NF_ACCEPT;

    uint8_t protocol = ip->protocol;
    uint16_t total_len = ntohs(ip->total_len);

    /* Sanity check: total_len must not exceed available buffer */
    if (total_len < sizeof(struct ip_header))
        return NF_ACCEPT;

    /* Locate L4 header */
    int ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
    if (ip_hdr_len < 20 || ip_hdr_len > 60)
        return NF_ACCEPT;

    int l4_off = ip_off + ip_hdr_len;
    /* Protect against integer overflow in l4_off */
    if (l4_off < 0 || l4_off > 65535)
        return NF_ACCEPT;

    uint16_t l4_payload_avail = (total_len > (uint16_t)ip_hdr_len)
                                ? (total_len - ip_hdr_len) : 0;
    (void)l4_payload_avail;  /* used implicitly below */

    int from_originator = 1;  /* direction relative to helper */

    /* ── TCP processing ─────────────────────────────────────────── */
    if (protocol == IPPROTO_TCP) {
        if (l4_off + (int)sizeof(struct tcp_header) < 0) return NF_ACCEPT;
        struct tcp_header *tcp = (struct tcp_header *)(buf + l4_off);

        uint16_t src_port = ntohs(tcp->src_port);
        uint16_t dst_port = ntohs(tcp->dst_port);

        /* Calculate TCP header length */
        int tcp_hdr_len = ((tcp->data_off >> 4) & 0x0F) * 4;
        if (tcp_hdr_len < 20 || tcp_hdr_len > 60)
            return NF_ACCEPT;

        int payload_off = l4_off + tcp_hdr_len;
        uint16_t tcp_payload_len = (total_len > (uint16_t)(ip_hdr_len + tcp_hdr_len))
                                   ? (total_len - ip_hdr_len - tcp_hdr_len) : 0;

        /* Only inspect packets with data (non-empty payload) and PSH or ACK */
        if (tcp_payload_len == 0)
            return NF_ACCEPT;

        const uint8_t *payload = buf + payload_off;

        /* ── Match helpers against (protocol, dst_port) ──────────── */
        /* Also match reverse direction (the source of a response
         * may be the helper's dst_port). */
        for (int i = 0; i < NF_HELPER_MAX; i++) {
            if (!nf_helpers[i].used) continue;
            if (nf_helpers[i].protocol != IPPROTO_TCP) continue;

            /* Match if src_port == helper_port (server responding)
             * OR dst_port == helper_port (client sending to server) */
            if (nf_helpers[i].dst_port == dst_port) {
                from_originator = 1;  /* client -> server */
                int verdict = nf_helpers[i].hook_fn(skb, ip, tcp,
                                                     payload, tcp_payload_len,
                                                     from_originator);
                if (verdict != NF_ACCEPT)
                    return verdict;
            } else if (nf_helpers[i].dst_port == src_port) {
                from_originator = 0;  /* server -> client */
                int verdict = nf_helpers[i].hook_fn(skb, ip, tcp,
                                                     payload, tcp_payload_len,
                                                     from_originator);
                if (verdict != NF_ACCEPT)
                    return verdict;
            }
        }
    }

    return NF_ACCEPT;
}

/* ══════════════════════════════════════════════════════════════════
 *               FTP ALG — PORT command parser
 * ══════════════════════════════════════════════════════════════════
 *
 * FTP PORT command format (RFC 959):
 *   "PORT h1,h2,h3,h4,p1,p2\r\n"
 * where h1.h2.h3.h4 is the IP address (octets) and p1*256+p2 is the port.
 *
 * When we see a PORT command from the client on the control connection,
 * we create an expected connection for the data channel.
 */

/* Parse IPv4 address and port from FTP PORT command ASCII.
 * Returns 1 on success, 0 on failure. */
static int parse_ftp_port(const uint8_t *data, uint16_t len,
                          uint32_t *ip_out, uint16_t *port_out)
{
    /* Minimum: "PORT 1,2,3,4,5,6\r\n" = 20 bytes */
    if (len < 20) return 0;

    /* Check for "PORT " prefix */
    if (data[0] != 'P' || data[1] != 'O' || data[2] != 'R' ||
        data[3] != 'T' || data[4] != ' ')
        return 0;

    /* Locate the start of the address string (after "PORT ") */
    const uint8_t *p = data + 5;
    const uint8_t *end = data + len;
    int octets[6];
    int count = 0;

    while (p < end && count < 6) {
        /* Skip whitespace between numbers */
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end || *p < '0' || *p > '9')
            break;

        /* Parse integer */
        int val = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            if (val > 255) return 0;  /* each octet must be 0-255 */
            p++;
        }
        octets[count++] = val;

        /* Expect comma or end of number sequence */
        if (count < 6) {
            if (p >= end || *p != ',') break;
            p++;
        }
    }

    if (count != 6)
        return 0;

    /* Build IP address (network byte order) */
    *ip_out = (uint32_t)((octets[0] << 24) | (octets[1] << 16) |
                         (octets[2] <<  8) |  octets[3]);

    /* Build port number */
    *port_out = (uint16_t)((octets[4] << 8) | octets[5]);

    return 1;
}

/* FTP helper hook — called for each data packet on the FTP control
 * connection (port 21).  Inspects the payload for PORT commands and
 * creates expected connection entries for the data channel. */
static int ftp_helper_hook(void *skb,
                           struct ip_header *ip,
                           struct tcp_header *tcp,
                           const uint8_t *payload,
                           uint16_t payload_len,
                           int from_originator)
{
    (void)skb;
    (void)tcp;

    /* Only process commands from the client (originator -> server) */
    if (!from_originator)
        return NF_ACCEPT;

    uint32_t server_ip = ntohl(ip->dst_ip);

    /* Scan the payload for PORT commands.
     * The payload may contain multiple lines (e.g., pipelined commands). */
    const uint8_t *scan = payload;
    uint16_t remaining = payload_len;

    while (remaining > 0) {
        /* Find end of line */
        const uint8_t *eol = scan;
        while (eol < payload + payload_len && *eol != '\r' && *eol != '\n')
            eol++;

        uint16_t line_len = (uint16_t)(eol - scan);

        /* Check if this line starts with "PORT " */
        if (line_len >= 20 &&
            scan[0] == 'P' && scan[1] == 'O' && scan[2] == 'R' &&
            scan[3] == 'T' && scan[4] == ' ') {

            uint32_t data_ip;
            uint16_t data_port;

            if (parse_ftp_port(scan, line_len, &data_ip, &data_port)) {
                /* Create expected connection for the data channel.
                 * In active FTP:
                 *   Client sends PORT with its own IP:port
                 *   Server connects BACK to client_ip:client_port
                 *   So expected: server_ip -> client_ip:data_port
                 *   (or more precisely, server connects to client at data_ip:data_port) */

                /* The expected data connection: server connects TO
                 * data_ip:data_port (the address the client specified). */
                int ret = nf_ct_expect_related(
                    NULL,                               /* master conn (FTP control) */
                    server_ip,                          /* src = server */
                    data_ip,                            /* dst = client-specified IP */
                    20,                                 /* src_port = FTP data port (20) */
                    data_port,                          /* dst_port = client-specified port */
                    IPPROTO_TCP);

                if (ret == 0) {
                    kprintf("[FTP-HELPER] PORT: expected conn %d.%d.%d.%d:%u -> "
                            "%d.%d.%d.%d:%u\n",
                            (server_ip >> 24) & 0xFF, (server_ip >> 16) & 0xFF,
                            (server_ip >> 8) & 0xFF, server_ip & 0xFF,
                            (unsigned)20,
                            (data_ip >> 24) & 0xFF, (data_ip >> 16) & 0xFF,
                            (data_ip >> 8) & 0xFF, data_ip & 0xFF,
                            (unsigned)data_port);
                }

                /* Also try PASV mode detection: "227 Entering Passive Mode..."
                 * In PASV, the server sends IP:port for client to connect.
                 * For now, we handle the more common active PORT mode. */
            }
        }

        /* Move past this line */
        scan = eol;
        while (scan < payload + payload_len && (*scan == '\r' || *scan == '\n'))
            scan++;

        remaining = (uint16_t)(payload + payload_len - scan);
    }

    return NF_ACCEPT;
}

/* ══════════════════════════════════════════════════════════════════
 *         Conntrack Integration — Mark RELATED connections
 * ══════════════════════════════════════════════════════════════════
 *
 * The conntrack system should be extended to call nf_ct_expect_lookup()
 * when a new connection is being tracked.  If the (src,dst,ports,proto)
 * tuple matches an expected entry, the connection is marked as
 * NF_CONN_RELATED instead of NF_CONN_NEW.
 *
 * This hook is called from nf_conntrack_in / nf_conntrack_out after
 * a new connection is created.  Returns 1 if the connection is RELATED
 * (expected), 0 otherwise.
 */

int nf_ct_check_expected(uint32_t src_ip, uint32_t dst_ip,
                          uint16_t src_port, uint16_t dst_port,
                          uint8_t protocol)
{
    struct nf_ct_expect *exp = nf_ct_expect_lookup(src_ip, dst_ip,
                                                    src_port, dst_port,
                                                    protocol);
    if (exp) {
        /* Consume the expected entry (one-shot) */
        exp->used = 0;
        nf_exp_count--;
        return 1;  /* This connection is RELATED */
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *                      Initialisation
 * ══════════════════════════════════════════════════════════════════ */

void nf_helper_init(void)
{
    /* Initialise expected connection table */
    memset(nf_exp_table, 0, sizeof(nf_exp_table));
    nf_exp_count = 0;
    spinlock_init(&nf_exp_lock);
    nf_exp_initialized = 1;

    /* Initialise helper table */
    memset(nf_helpers, 0, sizeof(nf_helpers));
    nf_helper_count = 0;

    /* Register the dispatcher hook at PRE_ROUTING and LOCAL_OUT */
    nf_register_hook(NF_INET_PRE_ROUTING, helper_dispatch_hook, 50);
    nf_register_hook(NF_INET_LOCAL_OUT,   helper_dispatch_hook, 50);

    /* Register FTP helper */
    nf_helper_register(IPPROTO_TCP, 21, ftp_helper_hook, "ftp");

    kprintf("[OK] Conntrack helpers initialized (expected=%d, helpers=%d)\n",
            NF_CT_EXPECT_MAX, NF_HELPER_MAX);
}
