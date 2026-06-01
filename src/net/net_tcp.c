/* net_tcp.c — TCP connection management */

#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "scheduler.h"

uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                                 const void *data, uint16_t data_len) {
    struct tcp_pseudo pseudo;
    pseudo.src_ip = htonl(src_ip);
    pseudo.dst_ip = htonl(dst_ip);
    pseudo.zero = 0;
    pseudo.protocol = protocol;
    pseudo.tcp_len = htons(data_len);

    uint32_t sum = 0;
    const uint8_t *pb = (const uint8_t *)&pseudo;
    for (int i = 0; i < (int)sizeof(pseudo); i += 2) {
        uint16_t w; __builtin_memcpy(&w, pb + i, 2); sum += w;
    }
    pb = (const uint8_t *)data;
    int len = data_len;
    while (len > 1) { uint16_t w; __builtin_memcpy(&w, pb, 2); sum += w; pb += 2; len -= 2; }
    if (len == 1) sum += *pb;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

void send_tcp(struct tcp_conn *conn, uint8_t flags, const void *data, uint16_t data_len) {
    uint8_t buf[1500];
    struct tcp_header *tcp = (struct tcp_header *)buf;
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq_num = htonl(conn->our_seq);
    tcp->ack_num = htonl(conn->their_seq);

    uint16_t opt_len = 0;
    uint8_t *opts = buf + sizeof(struct tcp_header);

    /* Add SACK-permitted option on SYN */
    if (flags & TCP_SYN) {
        opts[0] = 4;  /* SACK-permitted kind */
        opts[1] = 2;  /* length */
        opt_len = 2;
    }

    uint16_t hdr_len = sizeof(struct tcp_header) + opt_len;
    tcp->data_off = (uint8_t)((hdr_len / 4) << 4);
    tcp->flags = flags;
    tcp->window = htons(8192);
    if (data && data_len > 0)
        memcpy(buf + hdr_len, data, data_len);
    tcp->checksum = 0;
    tcp->checksum = net_transport_checksum(net_our_ip, conn->remote_ip, IP_PROTO_TCP,
                                           buf, hdr_len + data_len);

    send_ip(conn->remote_ip, IP_PROTO_TCP, buf, hdr_len + data_len);
}

static struct tcp_listener *find_listener(uint16_t port) {
    for (int i = 0; i < net_num_listeners; i++)
        if (net_listeners[i].port == port) return &net_listeners[i];
    return NULL;
}

static int find_conn(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state != TCP_CLOSED &&
            tcp_conns[i].remote_ip == remote_ip &&
            tcp_conns[i].remote_port == remote_port &&
            tcp_conns[i].local_port == local_port)
            return i;
    }
    return -1;
}

static int alloc_conn(void) {
    for (int i = 0; i < MAX_TCP_CONNS; i++)
        if (tcp_conns[i].state == TCP_CLOSED) return i;
    return -1;
}

void handle_tcp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(struct tcp_header)) return;
    struct tcp_header *tcp = (struct tcp_header *)payload;

    /* Verify TCP checksum */
    uint32_t csum_src = ntohl(ip_hdr->src_ip);
    uint32_t csum_dst = ntohl(ip_hdr->dst_ip);
    uint16_t saved_csum = tcp->checksum;
    if (saved_csum == 0) return; /* RFC 793: TCP requires a valid checksum; 0 is never valid for TCP */
    tcp->checksum = 0;
    if (net_transport_checksum(csum_src, csum_dst, IP_PROTO_TCP, payload, len) != saved_csum)
        return;

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t remote_ip = ntohl(ip_hdr->src_ip);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;
    uint16_t hdr_len = (tcp->data_off >> 4) * 4;
    if (hdr_len < sizeof(struct tcp_header) || hdr_len > len) return;
    uint16_t data_len = len - hdr_len;
    const uint8_t *data = payload + hdr_len;

    int conn_id = find_conn(remote_ip, src_port, dst_port);

    /* Parse TCP options — extract SACK blocks if present (only for established conns) */
    if (conn_id >= 0) {
        int opt_offset = sizeof(struct tcp_header);
        while (opt_offset + 1 < (int)hdr_len) {
            uint8_t kind = payload[opt_offset];
            if (kind == 0) break; /* End of options */
            if (kind == 1) { opt_offset++; continue; } /* NOP */
            if (opt_offset + 1 >= (int)hdr_len) break;
            uint8_t olen = payload[opt_offset + 1];
            if (olen < 2 || opt_offset + olen > (int)hdr_len) break;
            if (kind == 5) { /* SACK option */
                int num_blocks = (olen - 2) / 8;
                if (num_blocks > TCP_MAX_SACK_BLOCKS) num_blocks = TCP_MAX_SACK_BLOCKS;
                struct tcp_conn *sc = &tcp_conns[conn_id];
                memset(sc->sack_blocks, 0, sizeof(sc->sack_blocks));
                for (int sb = 0; sb < num_blocks; sb++) {
                    int off = opt_offset + 2 + sb * 8;
                    if (off + 8 <= (int)hdr_len) {
                        sc->sack_blocks[sb].left = ntohl(*(uint32_t*)(payload + off));
                        sc->sack_blocks[sb].right = ntohl(*(uint32_t*)(payload + off + 4));
                    }
                }
                sc->sack_pending = 1;
            } else if (kind == 19) { /* TCP MD5 Signature option */
                struct tcp_conn *sc = &tcp_conns[conn_id];
                sc->md5_enabled = 1;
                /* Option 19 format: kind(1) + len(1) + digest(16) */
                if (olen >= 18 && opt_offset + 2 + 16 <= (int)hdr_len) {
                    __builtin_memcpy(sc->md5_digest, payload + opt_offset + 2, 16);
                }
            } else if (kind == 34) { /* TCP Fast Open (TFO) Cookie option */
                struct tcp_conn *sc = &tcp_conns[conn_id];
                sc->tfo_cookie_present = 1;
                /* Option 34 format: kind(1) + len(1) + cookie(0-8 bytes) */
                int cookie_len = olen - 2;
                if (cookie_len > 8) cookie_len = 8;
                if (cookie_len > 0 && opt_offset + 2 + cookie_len <= (int)hdr_len) {
                    __builtin_memcpy(sc->tfo_cookie, payload + opt_offset + 2, cookie_len);
                }
            }
            opt_offset += olen;
        }
    }

    if (conn_id < 0 && (flags & TCP_SYN)) {
        struct tcp_listener *l = find_listener(dst_port);
        if (!l) {
            struct tcp_conn tmp;
            tmp.remote_ip = remote_ip;
            tmp.remote_port = src_port;
            tmp.local_port = dst_port;
            tmp.our_seq = ack;
            tmp.their_seq = seq + 1;
            send_tcp(&tmp, TCP_RST | TCP_ACK, NULL, 0);
            return;
        }

        conn_id = alloc_conn();
        if (conn_id < 0) return;

        struct tcp_conn *c = &tcp_conns[conn_id];
        c->state = TCP_SYN_RECEIVED;
        c->remote_ip = remote_ip;
        c->remote_port = src_port;
        c->local_port = dst_port;
        c->our_seq = 1000 + net_ip_id_counter * 1000;
        c->their_seq = seq + 1;
        c->their_window = ntohs(tcp->window);
        c->rxlen = 0;    /* reset stale state from previous use */
        c->rx_fin = 0;
        c->cwnd = 1;
        c->ssthresh = 65535;
        c->dupack_count = 0;
        c->srtt = 0;
        c->rttvar = 0;
        c->tx_unacked_len  = 0;
        c->tx_unacked_seq  = 0;
        c->last_send_tick  = 0;
        c->retrans_count   = 0;
        c->rto             = 30;   /* 3000ms initial RTO (100Hz) */
        c->tcp_nodelay     = 0;
        c->tcp_cork        = 0;
        c->keepalive       = 0;
        c->keepalive_interval = 500;
        c->keepalive_probes = 0;
        c->keepalive_probes_max = 3;
        c->last_activity_tick = 0;
        c->md5_enabled = 0;
        c->tfo_cookie_present = 0;
        memset(c->md5_digest, 0, sizeof(c->md5_digest));
        memset(c->tfo_cookie, 0, sizeof(c->tfo_cookie));
        memset(c->sack_blocks, 0, sizeof(c->sack_blocks));
        c->sack_pending = 0;

        send_tcp(c, TCP_SYN | TCP_ACK, NULL, 0);
        c->our_seq++;
        return;
    }

    if (conn_id < 0) return;

    struct tcp_conn *c = &tcp_conns[conn_id];
    struct tcp_listener *l = find_listener(c->local_port);

    if (c->state == TCP_SYN_SENT) {
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            c->their_seq = seq + 1;
            c->our_seq = ack;
            c->state = TCP_ESTABLISHED;
            send_tcp(c, TCP_ACK, NULL, 0);
        } else if (flags & TCP_RST) {
            c->state = TCP_CLOSED;
        }
        return;
    }

    if (c->state == TCP_SYN_RECEIVED) {
        if (flags & TCP_ACK) {
            c->state = TCP_ESTABLISHED;
            if (l && l->on_connect) {
                l->on_connect(conn_id);
            } else if (l && l->accept_count < ACCEPT_QUEUE_SIZE) {
                /* Accept-queue mode: enqueue the conn_id for net_tcp_accept() */
                l->accept_queue[l->accept_tail] = conn_id;
                l->accept_tail = (l->accept_tail + 1) % ACCEPT_QUEUE_SIZE;
                l->accept_count++;
            } else if (l) {
                /* Accept queue full — reject the connection */
                send_tcp(c, TCP_RST, NULL, 0);
                c->state = TCP_CLOSED;
            }
        }
        return;
    }

    if (c->state == TCP_ESTABLISHED) {
        if (flags & TCP_RST) {
            c->state = TCP_CLOSED;
            c->rx_fin = 1;
            if (l && l->on_close) l->on_close(conn_id);
            return;
        }

        /* ACK processing — congestion control, RTO, SACK */
        if ((flags & TCP_ACK)) {
            if (c->tx_unacked_len > 0) {
                /*** NEW ACK ***/
                if ((int32_t)(ack - c->last_ack) > 0) {
                    int acked_some = 0;
                    if ((int32_t)(ack - (c->tx_unacked_seq + c->tx_unacked_len)) >= 0) {
                        /* Fully ACKed — clear unacked buffer */
                        acked_some = 1;
                        c->tx_unacked_len = 0;
                        c->retrans_count  = 0;
                        c->dupack_count = 0;
                    } else if ((int32_t)(ack - c->tx_unacked_seq) > 0) {
                        /* Partial ACK */
                        acked_some = 1;
                        uint32_t acked_bytes = ack - c->tx_unacked_seq;
                        uint16_t acked16 = (uint16_t)(acked_bytes > 65535 ? 65535 : acked_bytes);
                        c->tx_unacked_len -= acked16;
                        if (c->tx_unacked_len > 0) {
                            memmove(c->tx_unacked_buf, c->tx_unacked_buf + acked16, c->tx_unacked_len);
                            c->tx_unacked_seq = ack;
                        } else {
                            c->tx_unacked_seq = 0;
                        }
                        c->retrans_count = 0;
                        c->dupack_count = 0;
                    }
                    c->last_ack = ack;

                    if (acked_some) {
                        /* Reno congestion control — advance cwnd per ACK */
                        if (c->cwnd < c->ssthresh)
                            c->cwnd++;       /* slow start */
                        else
                            c->cwnd++;       /* congestion avoidance (1/cwnd approx) */
                        if (c->cwnd > 1024) c->cwnd = 1024;

                        /* RTT estimation (Jacobson's algorithm) */
                        int32_t m = (int32_t)(timer_get_ticks() - c->last_send_tick);
                        if (m > 0) {
                            m = (m > 300) ? 300 : m;  /* clamp to 3s */
                            m = m * 8;  /* scale for srtt */
                            if (c->srtt == 0) {
                                c->srtt = m;
                                c->rttvar = m / 2;
                            } else {
                                int32_t delta = m - c->srtt;
                                c->srtt += delta / 8;
                                if (delta < 0) delta = -delta;
                                c->rttvar += (delta - c->rttvar) / 4;
                            }
                            /* RTO = srtt + 4 * rttvar, in ms */
                            int32_t rto_ms = (c->srtt + 4 * c->rttvar) / 8;
                            if (rto_ms < 100) rto_ms = 100;
                            if (rto_ms > 12000) rto_ms = 12000;
                            c->rto = (uint16_t)(rto_ms / 10 + 1);
                        }
                    }
                }
                /*** DUPLICATE ACK ***/
                else if ((int32_t)(ack - c->last_ack) == 0) {
                    c->dupack_count++;
                    if (c->dupack_count >= 3) {
                        /* Fast retransmit with SACK-based recovery */
                        uint32_t saved_seq = c->our_seq;
                        c->our_seq = c->tx_unacked_seq;
                        uint16_t remain = c->tx_unacked_len;
                        const uint8_t *rp = c->tx_unacked_buf;
                        while (remain > 0) {
                            uint16_t skip = 0;
                            for (int sb = 0; sb < TCP_MAX_SACK_BLOCKS; sb++) {
                                if (c->sack_blocks[sb].left == 0 &&
                                    c->sack_blocks[sb].right == 0) continue;
                                uint32_t base = c->tx_unacked_seq;
                                if ((int32_t)(base + remain - c->sack_blocks[sb].left) > 0 &&
                                    (int32_t)(base - c->sack_blocks[sb].right) < 0) {
                                    if (c->sack_blocks[sb].left > base &&
                                        c->sack_blocks[sb].left < base + remain) {
                                        uint16_t s = (uint16_t)(c->sack_blocks[sb].left - base);
                                        if (s > skip) skip = s;
                                    }
                                }
                            }
                            if (skip > 0) {
                                c->our_seq += skip;
                                rp += skip;
                                remain -= skip;
                            } else {
                                uint16_t chunk = remain > 1400 ? 1400 : remain;
                                send_tcp(c, TCP_PSH | TCP_ACK, rp, chunk);
                                c->our_seq += chunk;
                                rp += chunk;
                                remain -= chunk;
                            }
                        }
                        c->our_seq = saved_seq;
                        if (c->cwnd > 2) c->ssthresh = c->cwnd / 2;
                        else c->ssthresh = 2;
                        c->cwnd = c->ssthresh + 3;  /* RFC 5681 fast recovery */
                        c->dupack_count = 0;
                        c->last_send_tick = timer_get_ticks();
                    }
                }
            }
        }

        if (flags & TCP_FIN) {
            c->their_seq = seq + data_len + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->state = TCP_CLOSE_WAIT;
            c->rx_fin = 1;
            if (l && l->on_close) l->on_close(conn_id);
            return;
        }

        if (data_len > 0) {
            uint32_t expected = c->their_seq;
            /* Signed comparisons handle 32-bit sequence number wraparound */
            if ((int32_t)((seq + data_len) - expected) <= 0) {
                send_tcp(c, TCP_ACK, NULL, 0);
                return;
            }
            if ((int32_t)(seq - expected) > 0) {
                send_tcp(c, TCP_ACK, NULL, 0);
                return;
            }
            uint32_t skip = 0;
            if ((int32_t)(seq - expected) < 0) {
                skip = (uint32_t)(expected - seq);
                if (skip >= data_len) {
                    send_tcp(c, TCP_ACK, NULL, 0);
                    return;
                }
                data = (const uint8_t *)data + skip;
                data_len -= skip;
            }
            c->their_seq = expected + data_len;
            send_tcp(c, TCP_ACK, NULL, 0);
            /* Update activity for keepalive */
            c->last_activity_tick = timer_get_ticks();
            if (l && l->on_data) {
                l->on_data(conn_id, data, data_len);
            } else {
                int space = (int)sizeof(c->rxbuf) - c->rxlen;
                int copy = (int)data_len < space ? (int)data_len : space;
                if (copy > 0) {
                    memcpy(c->rxbuf + c->rxlen, data, copy);
                    c->rxlen += copy;
                }
            }
        }
        return;
    }

    if (c->state == TCP_FIN_WAIT) {
        if (flags & TCP_ACK) {
            c->state = TCP_FIN_WAIT_2;
        }
        if (flags & TCP_FIN) {
            c->their_seq = seq + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->state = TCP_TIME_WAIT;
            c->last_send_tick = timer_get_ticks();
        }
        return;
    }

    if (c->state == TCP_FIN_WAIT_2) {
        if (flags & TCP_FIN) {
            c->their_seq = seq + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->state = TCP_TIME_WAIT;
            c->last_send_tick = timer_get_ticks();
        }
        return;
    }

    if (c->state == TCP_TIME_WAIT) {
        /* Stay in TIME_WAIT; the periodic retransmit check cleans up after 2*MSL */
        return;
    }

    if (c->state == TCP_CLOSE_WAIT) {
        /* Application should call net_tcp_close to send FIN */
        if (flags & TCP_RST) {
            c->state = TCP_CLOSED;
        }
        return;
    }

    if (c->state == TCP_LAST_ACK) {
        if (flags & TCP_ACK) {
            c->state = TCP_CLOSED;
        }
        return;
    }
}

/* --- Outgoing TCP connect --- */
static uint16_t next_ephemeral_port = 49152;

int net_tcp_connect(uint32_t ip, uint16_t port) {
    int conn_id = alloc_conn();
    if (conn_id < 0) return -1;

    struct tcp_conn *c = &tcp_conns[conn_id];
    c->state = TCP_SYN_SENT;
    c->remote_ip = ip;
    c->remote_port = port;
    /* Pick an ephemeral port, avoiding collisions */
    int port_tries = 0;
    do {
        c->local_port = next_ephemeral_port++;
        if (next_ephemeral_port > 60000) next_ephemeral_port = 49152;
        port_tries++;
        /* Check if port is already in use by any connection */
        int port_in_use = 0;
        for (int i = 0; i < MAX_TCP_CONNS; i++) {
            if (i != conn_id && tcp_conns[i].state != TCP_CLOSED &&
                tcp_conns[i].local_port == c->local_port) {
                port_in_use = 1; break;
            }
        }
        if (!port_in_use) break;
    } while (port_tries < 1000);
    c->our_seq = 10000 + net_ip_id_counter * 1000;
    c->their_seq = 0;
    c->their_window = 0;
    c->rxlen = 0;
    c->rx_fin = 0;
    c->cwnd = 1;
    c->ssthresh = 65535;
    c->dupack_count = 0;
    c->srtt = 0;
    c->rttvar = 0;
    c->tx_unacked_len  = 0;
    c->tx_unacked_seq  = 0;
    c->last_send_tick  = 0;
    c->retrans_count   = 0;
    c->rto             = 30;
    c->tcp_nodelay     = 0;
    c->tcp_cork        = 0;
    c->keepalive       = 0;
    c->keepalive_interval = 500;
    c->keepalive_probes = 0;
    c->keepalive_probes_max = 3;
    c->last_activity_tick = 0;
    c->md5_enabled = 0;
    c->tfo_cookie_present = 0;
    memset(c->md5_digest, 0, sizeof(c->md5_digest));
    memset(c->tfo_cookie, 0, sizeof(c->tfo_cookie));
    memset(c->sack_blocks, 0, sizeof(c->sack_blocks));
    c->sack_pending = 0;

    send_tcp(c, TCP_SYN, NULL, 0);
    c->our_seq++;

    uint64_t start = timer_get_ticks();
    volatile uint32_t tries = 0;
    while (c->state == TCP_SYN_SENT) {
        net_poll();
        tries++;
        uint64_t now = timer_get_ticks();
        if (now != start && now - start > 500) {
            memset(c, 0, sizeof(*c));
            return -1;
        }
        if (tries > 5000000) {
            memset(c, 0, sizeof(*c));
            return -1;
        }
    }
    if (c->state != TCP_ESTABLISHED) {
        memset(c, 0, sizeof(*c));
        return -1;
    }
    return conn_id;
}

int net_tcp_recv(int conn_id, void *buf, uint16_t bufsize, int timeout_ticks) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -1;
    struct tcp_conn *c = &tcp_conns[conn_id];

    uint64_t start = timer_get_ticks();
    volatile uint32_t tries = 0;
    while (c->rxlen == 0 && !c->rx_fin) {
        net_poll();
        tries++;
        if (timeout_ticks > 0) {
            uint64_t now = timer_get_ticks();
            if (now != start && (int)(now - start) > timeout_ticks)
                break;
            if (tries > (uint32_t)timeout_ticks * 10000)
                break;
        }
    }
    int got = c->rxlen < bufsize ? c->rxlen : bufsize;
    if (got > 0) {
        memcpy(buf, c->rxbuf, got);
        int remain = c->rxlen - got;
        if (remain > 0)
            memmove(c->rxbuf, c->rxbuf + got, remain);
        c->rxlen = remain;
    }
    return got;
}

void net_tcp_listen(uint16_t port, tcp_connect_handler on_connect,
                    tcp_data_handler on_data, tcp_close_handler on_close) {
    if (net_num_listeners >= MAX_LISTENERS) return;
    struct tcp_listener *l = &net_listeners[net_num_listeners];
    l->port       = port;
    l->on_connect = on_connect;
    l->on_data    = on_data;
    l->on_close   = on_close;
    l->accept_head  = 0;
    l->accept_tail  = 0;
    l->accept_count = 0;
    net_num_listeners++;
}

void net_tcp_unlisten(uint16_t port) {
    for (int i = 0; i < net_num_listeners; i++) {
        if (net_listeners[i].port == port) {
            /* Compact the array */
            for (int j = i; j < net_num_listeners - 1; j++)
                net_listeners[j] = net_listeners[j + 1];
            net_num_listeners--;
            return;
        }
    }
}

int net_tcp_send(int conn_id, const void *data, uint16_t len) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -1;
    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state != TCP_ESTABLISHED) return -1;

    /* Limit to what fits in the retransmit buffer */
    uint16_t send_len = len;
    if (send_len > (uint16_t)sizeof(c->tx_unacked_buf))
        send_len = (uint16_t)sizeof(c->tx_unacked_buf);

    /* Buffer the full payload for potential retransmission */
    c->tx_unacked_seq = c->our_seq;
    memcpy(c->tx_unacked_buf, data, send_len);
    c->tx_unacked_len  = send_len;
    c->last_send_tick  = timer_get_ticks();
    c->retrans_count   = 0;
    c->dupack_count    = 0;
    c->last_activity_tick = timer_get_ticks();
    if (c->rto == 0) c->rto = 30;

    /* If TCP_CORK is set, buffer data but don't send yet (caller must uncork) */
    if (c->tcp_cork) {
        return 0;
    }

    /* TCP_NODELAY: send immediately, bypassing Nagle */
    if (c->tcp_nodelay) {
        const uint8_t *p = (const uint8_t *)data;
        uint16_t remaining = send_len;
        while (remaining > 0) {
            uint16_t chunk = remaining > 1400 ? 1400 : remaining;
            send_tcp(c, TCP_PSH | TCP_ACK, p, chunk);
            c->our_seq += chunk;
            p += chunk;
            remaining -= chunk;
        }
        return 0;
    }

    /* Default (Nagle's algorithm): send immediately if:
     * - we have no outstanding data, OR
     * - the data fits in a full MSS (MSS-based Nagle) */
    if (c->tx_unacked_len == 0 || send_len >= 1400) {
        const uint8_t *p = (const uint8_t *)data;
        uint16_t remaining = send_len;
        while (remaining > 0) {
            uint16_t chunk = remaining > 1400 ? 1400 : remaining;
            send_tcp(c, TCP_PSH | TCP_ACK, p, chunk);
            c->our_seq += chunk;
            p += chunk;
            remaining -= chunk;
        }
    }
    /* If Nagle delays the data, it stays buffered and will be sent
     * on the next poll cycle when the previous data is ACKed */
    return 0;
}

void net_tcp_close(int conn_id) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return;
    struct tcp_conn *c = &tcp_conns[conn_id];
    switch (c->state) {
        case TCP_ESTABLISHED:
            send_tcp(c, TCP_FIN | TCP_ACK, NULL, 0);
            c->our_seq++;
            c->state = TCP_FIN_WAIT;
            break;
        case TCP_CLOSE_WAIT:
            send_tcp(c, TCP_FIN | TCP_ACK, NULL, 0);
            c->our_seq++;
            c->state = TCP_LAST_ACK;
            break;
        case TCP_SYN_SENT:
        case TCP_SYN_RECEIVED:
            send_tcp(c, TCP_RST, NULL, 0);
            c->state = TCP_CLOSED;
            break;
        case TCP_FIN_WAIT:
        case TCP_FIN_WAIT_2:
        case TCP_LAST_ACK:
        case TCP_CLOSED:
        default:
            break;
    }
}

/* --- Blocking server accept --- */

int net_tcp_accept(uint16_t port, int timeout_ticks) {
    struct tcp_listener *l = find_listener(port);
    if (!l) return -1;

    uint64_t start = timer_get_ticks();
    while (l->accept_count == 0) {
        net_poll();
        scheduler_yield();  /* allow other processes to run while waiting */
        if (timeout_ticks > 0) {
            uint64_t now = timer_get_ticks();
            if (now != start && (int)(now - start) > timeout_ticks)
                return -1;
        }
    }
    int conn_id = l->accept_queue[l->accept_head];
    l->accept_head = (l->accept_head + 1) % ACCEPT_QUEUE_SIZE;
    l->accept_count--;
    return conn_id;
}

void net_conn_list(void (*cb)(uint16_t lport, uint32_t rip, uint16_t rport, int state)) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state != TCP_CLOSED)
            cb(tcp_conns[i].local_port, tcp_conns[i].remote_ip,
               tcp_conns[i].remote_port, (int)tcp_conns[i].state);
    }
}

/* Periodic retransmission check — called from timer every N ticks */
void net_tcp_check_retransmit(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        struct tcp_conn *c = &tcp_conns[i];

        /* Clean up TIME_WAIT connections after 2*MSL (100 ticks ≈ 1s) */
        if (c->state == TCP_TIME_WAIT) {
            if (now - c->last_send_tick >= 100)
                c->state = TCP_CLOSED;
            continue;
        }

        if (c->state != TCP_ESTABLISHED) continue;
        if (c->tx_unacked_len == 0) continue;
        if (now - c->last_send_tick < c->rto) continue;

        /* Give up after 5 retransmissions (RTO would be ~32 s at that point) */
        if (c->retrans_count >= 5) {
            c->state = TCP_CLOSED;
            c->rx_fin = 1;
            c->tx_unacked_len = 0;
            continue;
        }

        /* Retransmit using the saved sequence number, in MSS-sized chunks,
         * skipping data already reported as received via SACK. */
        uint32_t saved_seq = c->our_seq;
        c->our_seq = c->tx_unacked_seq;
        uint16_t remain = c->tx_unacked_len;
        const uint8_t *rp = c->tx_unacked_buf;

        /* Build a list of SACK-covered byte ranges to skip */
        while (remain > 0) {
            /* Determine how many bytes to skip based on SACK blocks */
            uint16_t skip = 0;
            for (int sb = 0; sb < TCP_MAX_SACK_BLOCKS; sb++) {
                if (c->sack_blocks[sb].left == 0 && c->sack_blocks[sb].right == 0)
                    continue;
                uint32_t seq_off = c->tx_unacked_seq;
                if ((int32_t)(seq_off + remain - c->sack_blocks[sb].left) > 0 &&
                    (int32_t)(seq_off - c->sack_blocks[sb].right) < 0) {
                    if (c->sack_blocks[sb].left > seq_off &&
                        c->sack_blocks[sb].left < seq_off + remain) {
                        uint16_t s = (uint16_t)(c->sack_blocks[sb].left - seq_off);
                        if (s > skip) skip = s;
                    }
                }
            }
            if (skip > 0) {
                c->our_seq += skip;
                rp += skip;
                remain -= skip;
            } else {
                uint16_t chunk = remain > 1400 ? 1400 : remain;
                send_tcp(c, TCP_PSH | TCP_ACK, rp, chunk);
                c->our_seq += chunk;
                rp += chunk;
                remain -= chunk;
            }
        }
        c->our_seq = saved_seq;

        c->last_send_tick = now;
        c->retrans_count++;
        /* Exponential back-off, cap at 64 s */
        c->rto = (c->rto * 2 > 6400) ? 6400 : (uint16_t)(c->rto * 2);
        /* Congestion control: halve cwnd */
        c->ssthresh = (c->cwnd / 2 < 2) ? 2 : c->cwnd / 2;
        c->cwnd = 1;
    }
}

/* ── Keepalive check ──────────────────────────────────────────── */

#define KEEPALIVE_INTERVAL_DEFAULT 500
#define KEEPALIVE_PROBES_MAX_DEFAULT 3

void net_tcp_set_keepalive(int conn_id, int keepalive) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return;
    struct tcp_conn *c = &tcp_conns[conn_id];
    c->keepalive = keepalive;
    if (keepalive) {
        c->keepalive_interval = KEEPALIVE_INTERVAL_DEFAULT;
        c->keepalive_probes = 0;
        c->keepalive_probes_max = KEEPALIVE_PROBES_MAX_DEFAULT;
        c->last_activity_tick = timer_get_ticks();
    }
}

int net_tcp_get_keepalive(int conn_id) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return 0;
    return tcp_conns[conn_id].keepalive;
}

void net_tcp_check_keepalive(void) {
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        struct tcp_conn *c = &tcp_conns[i];
        if (c->state != TCP_ESTABLISHED || !c->keepalive) continue;
        if (now - c->last_activity_tick >= c->keepalive_interval) {
            if (c->keepalive_probes >= c->keepalive_probes_max) {
                c->state = TCP_CLOSED;
                c->rx_fin = 1;
                continue;
            }
            uint32_t saved_seq = c->our_seq;
            c->our_seq = c->tx_unacked_seq > 0 ?
                         c->tx_unacked_seq - 1 : c->our_seq - 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            c->our_seq = saved_seq;
            c->keepalive_probes++;
            c->last_activity_tick = now;
        }
    }
}

int net_tcp_get_info(int conn_id, struct tcp_conn_info *info) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return -1;
    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_CLOSED) return -1;
    info->local_port = c->local_port;
    info->remote_ip = c->remote_ip;
    info->remote_port = c->remote_port;
    info->state = (int)c->state;
    info->cwnd = c->cwnd;
    info->ssthresh = c->ssthresh;
    info->last_send_tick = c->last_send_tick;
    info->retrans_count = c->retrans_count;
    return 0;
}
