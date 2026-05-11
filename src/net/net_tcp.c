/* net_tcp.c — TCP connection management */

#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *tcp_data, uint16_t tcp_len) {
    struct tcp_pseudo pseudo;
    pseudo.src_ip = htonl(src_ip);
    pseudo.dst_ip = htonl(dst_ip);
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_TCP;
    pseudo.tcp_len = htons(tcp_len);

    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)&pseudo;
    for (int i = 0; i < (int)(sizeof(pseudo) / 2); i++) sum += p[i];
    p = (const uint16_t *)tcp_data;
    int len = tcp_len;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~(uint16_t)sum;
}

void send_tcp(struct tcp_conn *conn, uint8_t flags, const void *data, uint16_t data_len) {
    static uint8_t buf[1500];
    struct tcp_header *tcp = (struct tcp_header *)buf;
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = htons(conn->local_port);
    tcp->dst_port = htons(conn->remote_port);
    tcp->seq_num = htonl(conn->our_seq);
    tcp->ack_num = htonl(conn->their_seq);
    tcp->data_off = (5 << 4);
    tcp->flags = flags;
    tcp->window = htons(8192);
    if (data && data_len > 0)
        memcpy(buf + sizeof(struct tcp_header), data, data_len);
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(net_our_ip, conn->remote_ip, buf, sizeof(struct tcp_header) + data_len);

    send_ip(conn->remote_ip, IP_PROTO_TCP, buf, sizeof(struct tcp_header) + data_len);
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

    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t remote_ip = ntohl(ip_hdr->src_ip);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t flags = tcp->flags;
    uint16_t hdr_len = (tcp->data_off >> 4) * 4;
    uint16_t data_len = len - hdr_len;
    const uint8_t *data = payload + hdr_len;

    int conn_id = find_conn(remote_ip, src_port, dst_port);

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
            if (l && l->on_connect) l->on_connect(conn_id);
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

        if (flags & TCP_FIN) {
            c->their_seq = seq + data_len + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
            send_tcp(c, TCP_FIN | TCP_ACK, NULL, 0);
            c->our_seq++;
            c->state = TCP_CLOSE_WAIT;
            c->rx_fin = 1;
            if (l && l->on_close) l->on_close(conn_id);
            return;
        }

        if (data_len > 0) {
            uint32_t expected = c->their_seq;
            if (seq + data_len <= expected) {
                send_tcp(c, TCP_ACK, NULL, 0);
                return;
            }
            if (seq > expected) {
                send_tcp(c, TCP_ACK, NULL, 0);
                return;
            }
            uint16_t skip = 0;
            if (seq < expected) {
                skip = (uint16_t)(expected - seq);
                data = (const uint8_t *)data + skip;
                data_len -= skip;
            }
            c->their_seq = expected + data_len;
            send_tcp(c, TCP_ACK, NULL, 0);
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
        if (flags & TCP_FIN) {
            c->their_seq = seq + 1;
            send_tcp(c, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_ACK) {
            c->state = TCP_CLOSED;
        }
        return;
    }

    if (c->state == TCP_CLOSE_WAIT) {
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
    c->local_port = next_ephemeral_port++;
    if (next_ephemeral_port > 60000) next_ephemeral_port = 49152;
    c->our_seq = 10000 + net_ip_id_counter * 1000;
    c->their_seq = 0;
    c->their_window = 0;
    c->rxlen = 0;
    c->rx_fin = 0;

    send_tcp(c, TCP_SYN, NULL, 0);
    c->our_seq++;

    uint64_t start = timer_get_ticks();
    volatile uint32_t tries = 0;
    while (c->state == TCP_SYN_SENT) {
        net_poll();
        tries++;
        uint64_t now = timer_get_ticks();
        if (now != start && now - start > 500) {
            c->state = TCP_CLOSED;
            return -1;
        }
        if (tries > 5000000) {
            c->state = TCP_CLOSED;
            return -1;
        }
    }
    if (c->state != TCP_ESTABLISHED) {
        c->state = TCP_CLOSED;
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
    net_listeners[net_num_listeners].port = port;
    net_listeners[net_num_listeners].on_connect = on_connect;
    net_listeners[net_num_listeners].on_data = on_data;
    net_listeners[net_num_listeners].on_close = on_close;
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

    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        uint16_t chunk = len > 1400 ? 1400 : len;
        send_tcp(c, TCP_PSH | TCP_ACK, p, chunk);
        c->our_seq += chunk;
        p += chunk;
        len -= chunk;
    }
    return 0;
}

void net_tcp_close(int conn_id) {
    if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) return;
    struct tcp_conn *c = &tcp_conns[conn_id];
    if (c->state == TCP_ESTABLISHED) {
        send_tcp(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->our_seq++;
        c->state = TCP_FIN_WAIT;
    }
}
