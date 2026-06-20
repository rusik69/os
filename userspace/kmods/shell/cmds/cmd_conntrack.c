/* cmd_conntrack.c — Connection tracking display */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "types.h"

/* ── Core conntrack definitions (mirror of netfilter.h) ──────── */
#define IPPROTO_ICMP  1
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

#define TCP_CONN_NONE       0
#define TCP_CONN_SYN_SENT   1
#define TCP_CONN_SYN_RECV   2
#define TCP_CONN_ESTABLISHED 3
#define TCP_CONN_FIN_WAIT_1 4
#define TCP_CONN_FIN_WAIT_2 5
#define TCP_CONN_CLOSE_WAIT 6
#define TCP_CONN_CLOSING    7
#define TCP_CONN_LAST_ACK   8
#define TCP_CONN_TIME_WAIT  9
#define TCP_CONN_MAX_STATE  10

#define UDP_CONN_NONE      0
#define UDP_CONN_UNREPLIED 1
#define UDP_CONN_ASSURED   2

#define ICMP_CONN_NONE    0
#define ICMP_CONN_REQUEST 1
#define ICMP_CONN_REPLY   2
#define ICMP_CONN_ERROR   3

#define NF_CONNTRACK_MAX  256

struct nf_conn {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  protocol;
    uint8_t  proto_state;
    uint8_t  tcp_flags_seen;
    uint16_t tcp_wscale;
    uint8_t  orig_saw_reply;
    uint8_t  reply_saw_orig;
    uint32_t timeout_ticks;
    uint64_t last_seen;
    uint8_t  timeout_idx;
    uint8_t  used;
    uint64_t packets;
    uint64_t bytes;
    uint64_t packets_reply;
    uint64_t bytes_reply;
    uint32_t mark;
};

struct nf_conntrack_stats {
    uint64_t total_lookups;
    uint64_t total_creations;
    uint64_t total_destroys;
    uint64_t total_expired;
    uint64_t table_full_errors;
    uint64_t current_active;
    uint64_t max_active;
    uint64_t tcp_states[TCP_CONN_MAX_STATE];
};

/* ── External conntrack API (from net/conntrack.c) ───────────── */
int  nf_conntrack_dump(struct nf_conn *buf, int max);
void nf_conntrack_stats_get(struct nf_conntrack_stats *stats);

/* ── Helpers ─────────────────────────────────────────────────── */
static const char *tcp_state_name(uint8_t state)
{
    switch (state) {
        case TCP_CONN_NONE:        return "NONE";
        case TCP_CONN_SYN_SENT:    return "SYN_SENT";
        case TCP_CONN_SYN_RECV:    return "SYN_RECV";
        case TCP_CONN_ESTABLISHED: return "ESTABLISHED";
        case TCP_CONN_FIN_WAIT_1:  return "FIN_WAIT_1";
        case TCP_CONN_FIN_WAIT_2:  return "FIN_WAIT_2";
        case TCP_CONN_CLOSE_WAIT:  return "CLOSE_WAIT";
        case TCP_CONN_CLOSING:     return "CLOSING";
        case TCP_CONN_LAST_ACK:    return "LAST_ACK";
        case TCP_CONN_TIME_WAIT:   return "TIME_WAIT";
        default:                   return "UNKNOWN";
    }
}

static const char *udp_state_name(uint8_t state)
{
    switch (state) {
        case UDP_CONN_NONE:       return "NONE";
        case UDP_CONN_UNREPLIED:  return "UNREPLIED";
        case UDP_CONN_ASSURED:    return "ASSURED";
        default:                  return "UNKNOWN";
    }
}

static const char *icmp_state_name(uint8_t state)
{
    switch (state) {
        case ICMP_CONN_NONE:    return "NONE";
        case ICMP_CONN_REQUEST: return "REQUEST";
        case ICMP_CONN_REPLY:   return "REPLY";
        case ICMP_CONN_ERROR:   return "ERROR";
        default:                return "UNKNOWN";
    }
}

static const char *proto_name(uint8_t proto)
{
    switch (proto) {
        case IPPROTO_TCP:  return "tcp";
        case IPPROTO_UDP:  return "udp";
        case IPPROTO_ICMP: return "icmp";
        default:           return "unknown";
    }
}

static const char *conn_state_str(uint8_t proto, uint8_t proto_state)
{
    switch (proto) {
        case IPPROTO_TCP:  return tcp_state_name(proto_state);
        case IPPROTO_UDP:  return udp_state_name(proto_state);
        case IPPROTO_ICMP: return icmp_state_name(proto_state);
        default:           return "?";
    }
}

void cmd_conntrack(const char *args)
{
    struct nf_conn dump_buf[64];
    struct nf_conntrack_stats stats;
    int count;

    if (!args || !*args) {
        kprintf("Usage: conntrack -L\n");
        kprintf("Connection tracking table.\n");
        return;
    }

    while (*args == ' ') args++;

    if (strncmp(args, "-L", 2) == 0) {
        count = nf_conntrack_dump(dump_buf, 64);
        nf_conntrack_stats_get(&stats);

        if (count == 0) {
            kprintf("conntrack: connection tracking table empty\n");
        } else {
            kprintf("Connection tracking table (%d entries):\n", count);
            kprintf("Proto  SrcIP:Port              DstIP:Port              State\n");
            kprintf("-----  ----------------------  ----------------------  -------------\n");
            for (int i = 0; i < count; i++) {
                struct nf_conn *c = &dump_buf[i];
                kprintf("%-5s  %u.%u.%u.%u:%-5u        %u.%u.%u.%u:%-5u        %s\n",
                        proto_name(c->protocol),
                        (unsigned int)((c->src_ip >> 24) & 0xFF),
                        (unsigned int)((c->src_ip >> 16) & 0xFF),
                        (unsigned int)((c->src_ip >> 8) & 0xFF),
                        (unsigned int)(c->src_ip & 0xFF),
                        (unsigned int)c->src_port,
                        (unsigned int)((c->dst_ip >> 24) & 0xFF),
                        (unsigned int)((c->dst_ip >> 16) & 0xFF),
                        (unsigned int)((c->dst_ip >> 8) & 0xFF),
                        (unsigned int)(c->dst_ip & 0xFF),
                        (unsigned int)c->dst_port,
                        conn_state_str(c->protocol, c->proto_state));
            }
        }

        kprintf("\nConntrack statistics:\n");
        kprintf("  Active: %lu, Max: %lu\n",
                (unsigned long)stats.current_active,
                (unsigned long)stats.max_active);
        kprintf("  Created: %lu, Destroyed: %lu, Expired: %lu\n",
                (unsigned long)stats.total_creations,
                (unsigned long)stats.total_destroys,
                (unsigned long)stats.total_expired);
    } else {
        kprintf("conntrack: unknown option '%s'\n", args);
        kprintf("Usage: conntrack -L\n");
    }
}
