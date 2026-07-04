/* sctp.c — Stream Control Transmission Protocol (RFC 4960) */

#include "sctp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"
#include "timer.h"

static struct sctp_assoc sctp_assocs[SCTP_MAX_ASSOCS];
static spinlock_t sctp_lock;
static int sctp_initialized = 0;

void sctp_init(void)
{
    if (sctp_initialized) return;
    spinlock_init(&sctp_lock);
    memset(sctp_assocs, 0, sizeof(sctp_assocs));
    sctp_initialized = 1;
    kprintf("[OK] SCTP: Stream Control Transmission Protocol initialized\n");
}

static int sctp_find_free(void)
{
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (!sctp_assocs[i].used)
            return i;
    }
    return -1;
}

static struct sctp_assoc *sctp_find_by_fd(int fd)
{
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (sctp_assocs[i].used && sctp_assocs[i].local_port == (uint16_t)fd)
            return &sctp_assocs[i];
    }
    return NULL;
}

int sctp_create(int fd, int type)
{
    (void)type;
    if (!sctp_initialized) return -ENOSYS;

    spinlock_acquire(&sctp_lock);
    int slot = sctp_find_free();
    if (slot < 0) {
        spinlock_release(&sctp_lock);
        return -ENOMEM;
    }

    struct sctp_assoc *a = &sctp_assocs[slot];
    memset(a, 0, sizeof(*a));
    a->used = 1;
    a->state = SCTP_STATE_CLOSED;
    /* Use fd as local port for identification */
    a->local_port = (uint16_t)fd;
    a->num_in_streams = SCTP_MAX_STREAMS;
    a->num_out_streams = SCTP_MAX_STREAMS;
    /* Initialize TSN tracking */
    a->initial_tsn = 100;
    a->next_tsn = 100;
    a->cum_tsn_ack = 0;
    a->last_rcvd_tsn = 0;
    a->rwnd = 65536;
    a->peer_rwnd = 65536;
    /* Initialize stream sequence numbers */
    for (int i = 0; i < SCTP_MAX_STREAMS; i++) {
        a->in_streams[i].id = (uint16_t)i;
        a->in_streams[i].in_seq = 0;
        a->in_streams[i].out_seq = 0;
        a->out_streams[i].id = (uint16_t)i;
        a->out_streams[i].in_seq = 0;
        a->out_streams[i].out_seq = 0;
    }

    /* Initialize heartbeat reachability detection */
    sctp_heartbeat_init(a);

    spinlock_release(&sctp_lock);
    return 0;
}

int sctp_bind(int fd, uint16_t port)
{
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }
    a->local_port = port;
    spinlock_release(&sctp_lock);
    return 0;
}

int sctp_connect(int fd, uint32_t ip, uint16_t port)
{
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    a->peer_ip = ip;
    a->peer_port = port;
    a->local_tag = 0xDEADBEEF;  /* Random tag */
    a->state = SCTP_STATE_COOKIE_WAIT;

    /* Register initial transport for multi-homing */
    sctp_transport_add(a, ip, port);

    /* Build and send SCTP INIT chunk */
    uint8_t pkt[256];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    memset(sh, 0, sizeof(*sh));
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(port);
    sh->vtag = 0;  /* Tag=0 in INIT */
    sh->checksum = 0;

    /* INIT chunk */
    struct sctp_chunk *chunk = (struct sctp_chunk *)(pkt + sizeof(*sh));
    chunk->type = SCTP_INIT;
    chunk->flags = 0;
    chunk->length = htons(20);  /* Init chunk = 20 bytes */
    /* Init params follow: initiate_tag, a_rwnd, num_out_streams, num_in_streams, initial_tsn */
    uint32_t *init_params = (uint32_t *)(pkt + sizeof(*sh) + sizeof(*chunk));
    init_params[0] = htonl(a->local_tag);
    init_params[1] = htonl(65536); /* a_rwnd */
    init_params[2] = htons(a->num_out_streams);
    init_params[3] = htons(a->num_in_streams);
    init_params[4] = htonl(100); /* initial TSN */

    uint16_t pkt_len = sizeof(*sh) + sizeof(*chunk) + 20;
    send_ip(ip, IPPROTO_SCTP, pkt, pkt_len);

    a->tx_packets++;
    spinlock_release(&sctp_lock);
    return 0;
}

int sctp_send(int fd, const void *data, uint16_t len, uint16_t stream_id)
{
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a || a->state != SCTP_STATE_ESTABLISHED) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    if (stream_id >= SCTP_MAX_STREAMS)
        stream_id = 0;

    /* Build DATA chunk with TSN, stream info, and payload */
    uint16_t data_chunk_size = sizeof(struct sctp_data_hdr) + len;
    uint8_t pkt[sizeof(struct sctp_header) + data_chunk_size];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(a->peer_port);
    sh->vtag = htonl(a->peer_tag);

    struct sctp_data_hdr *dh = (struct sctp_data_hdr *)(pkt + sizeof(*sh));
    dh->hdr.type   = SCTP_DATA;
    dh->hdr.flags  = SCTP_DATA_LAST_FRAG;  /* B=1, E=1, U=0 — complete msg */
    dh->hdr.length = htons(data_chunk_size);
    dh->tsn        = htonl(sctp_tsn_alloc(a));
    dh->stream_id  = htons(stream_id);
    dh->stream_seq = htons(a->out_streams[stream_id].out_seq);
    dh->ppid       = 0;
    memcpy(pkt + sizeof(*sh) + sizeof(*dh), data, len);

    a->out_streams[stream_id].out_seq++;
    uint16_t pkt_len = sizeof(*sh) + data_chunk_size;
    sh->checksum = 0;  /* CRC32c would go here */

    send_ip(a->transports[a->primary_path].addr, IPPROTO_SCTP, pkt, pkt_len);
    a->tx_packets++;

    kprintf("sctp: sent DATA tsn=%u stream=%u seq=%u len=%u\n",
            ntohl(dh->tsn), stream_id,
            ntohs(dh->stream_seq), len);

    /* Check if heartbeat is needed on idle transports */
    sctp_heartbeat_check(a);

    spinlock_release(&sctp_lock);
    return (int)len;
}

int sctp_recv(int fd, void *buf, uint16_t maxlen, uint16_t *stream_id)
{
	spinlock_acquire(&sctp_lock);
	struct sctp_assoc *a = sctp_find_by_fd(fd);
	if (!a) {
		spinlock_release(&sctp_lock);
		return -EINVAL;
	}

	/* Find stream with ready data */
	int sid = -1;
	if (stream_id && *stream_id < SCTP_MAX_STREAMS) {
		/* Specific stream requested */
		if (a->in_streams[*stream_id].ready)
			sid = *stream_id;
	} else {
		/* Scan all streams for any pending data */
		for (int i = 0; i < SCTP_MAX_STREAMS; i++) {
			if (a->in_streams[i].ready) {
				sid = i;
				break;
			}
		}
	}

	if (sid < 0) {
		/* Fall back to shared rcvbuf (legacy) */
		if (a->rcvlen == 0) {
			spinlock_release(&sctp_lock);
			return -EAGAIN;
		}
		int copy_len = (a->rcvlen < maxlen) ? a->rcvlen : maxlen;
		memcpy(buf, a->rcvbuf, copy_len);
		a->rcvlen = 0;
		spinlock_release(&sctp_lock);
		return copy_len;
	}

	struct sctp_stream *s = &a->in_streams[sid];
	int copy_len = (s->ready_len < maxlen) ? s->ready_len : maxlen;
	memcpy(buf, s->ready_buf, copy_len);
	if (stream_id)
		*stream_id = (uint16_t)sid;
	s->ready = 0;
	s->ready_len = 0;

	spinlock_release(&sctp_lock);
	return copy_len;
}

void sctp_close(int fd)
{
    spinlock_acquire(&sctp_lock);
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (sctp_assocs[i].used && sctp_assocs[i].local_port == (uint16_t)fd) {
            sctp_transport_remove_all(&sctp_assocs[i]);
            memset(&sctp_assocs[i], 0, sizeof(struct sctp_assoc));
            break;
        }
    }
    spinlock_release(&sctp_lock);
}

int sctp_is_valid_fd(int fd)
{
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    spinlock_release(&sctp_lock);
    return a != NULL;
}

void handle_sctp(uint32_t src_ip, uint32_t dst_ip,
                 const uint8_t *payload, uint16_t len)
{
    (void)dst_ip;
    if (len < sizeof(struct sctp_header)) return;

    const struct sctp_header *sh = (const struct sctp_header *)payload;
    uint16_t dst_port = ntohs(sh->dst_port);

    spinlock_acquire(&sctp_lock);

    /* Find matching association */
    struct sctp_assoc *a = NULL;
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (sctp_assocs[i].used && sctp_assocs[i].local_port == dst_port) {
            a = &sctp_assocs[i];
            break;
        }
    }
    if (!a) {
        spinlock_release(&sctp_lock);
        return;
    }

    a->rx_packets++;
    const struct sctp_chunk *chunk = (const struct sctp_chunk *)(payload + sizeof(*sh));
    uint16_t remaining = len - sizeof(*sh);

    while (remaining >= sizeof(*chunk)) {
        uint16_t chunk_len = ntohs(chunk->length);
        if (chunk_len < sizeof(*chunk) || chunk_len > remaining) break;

        switch (chunk->type) {
        case SCTP_INIT:
            /* Delegate to state machine for proper INIT-ACK + cookie */
            sctp_sm_handle_init(a, src_ip, sh, chunk,
                                chunk_len);
            break;

        case SCTP_INIT_ACK:
            /* Delegate to state machine for COOKIE-ECHO */
            sctp_sm_handle_init_ack(a, src_ip, sh, chunk,
                                    chunk_len);
            break;

        case SCTP_COOKIE_ECHO:
            sctp_sm_handle_cookie_echo(a, src_ip, sh, chunk,
                                       chunk_len);
            break;

        case SCTP_COOKIE_ACK:
            sctp_sm_handle_cookie_ack(a, sh);
            break;

        case SCTP_HEARTBEAT: {
            /* HEARTBEAT received — respond with HEARTBEAT-ACK */
            uint16_t hb_info_len = chunk_len - sizeof(*chunk);
            if (hb_info_len < 4) break; /* Need at least hb_time */

            /* Build HEARTBEAT-ACK chunk */
            uint8_t hb_ack_pkt[sizeof(struct sctp_header) + chunk_len];
            struct sctp_header *hbo = (struct sctp_header *)hb_ack_pkt;
            memset(hbo, 0, sizeof(*hbo));
            hbo->src_port = sh->dst_port; /* Swap ports */
            hbo->dst_port = sh->src_port;
            hbo->vtag = htonl(a->peer_tag);
            hbo->checksum = 0;

            struct sctp_chunk *ack_chunk = (struct sctp_chunk *)(hb_ack_pkt + sizeof(*hbo));
            ack_chunk->type = SCTP_HEARTBEAT_ACK;
            ack_chunk->flags = 0;
            ack_chunk->length = htons(chunk_len);
            memcpy((uint8_t *)(ack_chunk + 1), (const uint8_t *)(chunk + 1), hb_info_len);

            uint16_t ack_pkt_len = sizeof(*hbo) + chunk_len;
            /* Send HEARTBEAT-ACK back to the source (may be alternate path) */
            send_ip(src_ip, IPPROTO_SCTP, hb_ack_pkt, ack_pkt_len);
            a->tx_packets++;

            /* Record activity on this transport */
            int tp_idx = sctp_transport_find(a, src_ip);
            if (tp_idx >= 0)
                a->transports[tp_idx].last_used = timer_get_ticks();
            kprintf("sctp: HEARTBEAT-ACK sent to %d.%d.%d.%d:%u\n",
                    (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                    (src_ip >> 8) & 0xFF, src_ip & 0xFF,
                    ntohs(sh->src_port));
            break;
        }

        case SCTP_HEARTBEAT_ACK: {
            /* HEARTBEAT-ACK received — peer is alive on this path */
            uint16_t hb_ack_total = chunk_len;
            uint16_t hb_ack_data = hb_ack_total - sizeof(*chunk);
            const uint8_t *hb_data = (const uint8_t *)(chunk + 1);

            /* Parse Heartbeat Info parameter (Type=1) */
            uint32_t sent_time = 0;
            if (hb_ack_data >= 8 && hb_data[0] == 0 && hb_data[1] == SCTP_PARAM_HB_INFO) {
                uint16_t param_len = ((uint16_t)hb_data[2] << 8) | hb_data[3];
                if (param_len >= 8) {
                    sent_time = ((uint32_t)hb_data[4] << 24) |
                                ((uint32_t)hb_data[5] << 16) |
                                ((uint32_t)hb_data[6] << 8)  |
                                (uint32_t)hb_data[7];
                }
            }

            /* Find transport by source IP */
            int tp_idx = sctp_transport_find(a, src_ip);
            if (tp_idx >= 0) {
                struct sctp_transport *tp = &a->transports[tp_idx];

                /* Calculate RTT if we have the sent timestamp */
                if (sent_time > 0 && tp->hb_pending &&
                    tp->hb_last_sent == sent_time) {
                    uint64_t now = timer_get_ticks();
                    uint32_t rtt_sample = (uint32_t)now - sent_time;
                    /* Exponentially weighted moving average (alpha=0.125) */
                    if (tp->rtt == 0) {
                        tp->rtt = rtt_sample;
                    } else {
                        tp->rtt = (uint16_t)((uint32_t)tp->rtt * 7 / 8 +
                                              rtt_sample / 8);
                    }
                    /* Update RTO: 2 * RTT (simplified RFC 4960 §6.3.1) */
                    if (tp->rtt > 0) {
                        tp->rto = (uint32_t)tp->rtt * 2;
                        if (tp->rto < 1000) tp->rto = 1000;
                    }
                    tp->hb_pending = 0;

                    kprintf("sctp: HEARTBEAT-ACK RTT %u ms on "
                            NIPQUAD_FMT ":%u\n",
                            rtt_sample / 100 * 10,
                            NIPQUAD(tp->addr), tp->port);
                }

                /* Path recovery: mark transport active */
                if (!tp->active) {
                    tp->active = 1;
                    kprintf("sctp: path " NIPQUAD_FMT ":%u "
                            "RECOVERED (HEARTBEAT-ACK)\n",
                            NIPQUAD(tp->addr), tp->port);
                }
                tp->hb_timeout_count = 0;
                tp->last_used = timer_get_ticks();
            }

            kprintf("sctp: HEARTBEAT-ACK from " NIPQUAD_FMT ":%u\n",
                    NIPQUAD(src_ip), ntohs(sh->src_port));
            break;
        }

        case SCTP_DATA: {
            /* Use TSN-aware DATA handler */
            const struct sctp_data_hdr *dh =
                (const struct sctp_data_hdr *)chunk;
            sctp_tsn_rcv_data(a, src_ip,
                              ntohl(sh->vtag),
                              dh, chunk_len);
            break;
        }

        case SCTP_ABORT:
            a->state = SCTP_STATE_CLOSED;
            break;

        case SCTP_SHUTDOWN:
            sctp_sm_handle_shutdown(a, src_ip, sh, chunk,
                                     chunk_len);
            break;

        case SCTP_SHUTDOWN_ACK:
            sctp_sm_handle_shutdown_ack(a, src_ip, sh, chunk,
                                        chunk_len);
            break;

        case SCTP_SHUTDOWN_COMPLETE:
            sctp_sm_handle_shutdown_complete(a, src_ip, sh, chunk,
                                             chunk_len);
            break;

        case SCTP_SACK: {
            /* Process SACK to update cum_tsn_ack */
            const struct sctp_sack_hdr *sack =
                (const struct sctp_sack_hdr *)chunk;
            sctp_tsn_process_sack(a, sack, chunk_len);
            break;
        }

        case SCTP_ASCONF: {
            sctp_handle_asconf(a, src_ip, chunk, chunk_len);
            /* Update/receive source as a valid transport if not yet known */
            if (a->peer_ip != src_ip) {
                /* Record incoming packet from alternate address */
                int tidx = sctp_transport_find(a, src_ip);
                if (tidx < 0) {
                    kprintf("sctp: ASCONF from new address " NIPQUAD_FMT "\n",
                            NIPQUAD(src_ip));
                }
            }
            break;
        }

        case SCTP_ASCONF_ACK:
            kprintf("sctp: ASCONF-ACK from " NIPQUAD_FMT "\n",
                    NIPQUAD(src_ip));
            break;
        default:
            break;
        }

        remaining -= chunk_len;
        chunk = (const struct sctp_chunk *)((const uint8_t *)chunk + chunk_len);
    }

    /* Check all transports for heartbeat requirements */
    sctp_heartbeat_check(a);

    spinlock_release(&sctp_lock);
}

EXPORT_SYMBOL(sctp_init);
EXPORT_SYMBOL(sctp_create);
EXPORT_SYMBOL(sctp_bind);
EXPORT_SYMBOL(sctp_connect);
EXPORT_SYMBOL(sctp_send);
EXPORT_SYMBOL(sctp_recv);
EXPORT_SYMBOL(sctp_close);
/* ── Implement: sctp_recvmsg ────────────────── */
int sctp_recvmsg(int fd, void *buf, uint16_t maxlen, uint16_t *stream_id, uint16_t *ppid, int flags)
{
	(void)flags;
	/* Delegate to sctp_recv for stream_id delivery */
	int ret = sctp_recv(fd, buf, maxlen, stream_id);
	if (ret > 0 && ppid && stream_id && *stream_id < SCTP_MAX_STREAMS) {
		/* Look up PPID from per-stream ready metadata.
		 * sctp_recv consumed the ready flag, but the assoc
		 * still has the last PPID cached for this stream. */
		spinlock_acquire(&sctp_lock);
		struct sctp_assoc *a = sctp_find_by_fd(fd);
		if (a)
			*ppid = (uint16_t)a->in_streams[*stream_id].ready_ppid;
		spinlock_release(&sctp_lock);
	}
	return ret;
}

/* ── Implement: sctp_sendmsg ────────────────── */
int sctp_sendmsg(int fd, const void *data, uint16_t len, uint32_t to_ip, uint16_t to_port, uint16_t stream_id, uint32_t ppid, int flags)
{
    (void)to_ip;
    (void)to_port;
    (void)ppid;
    (void)flags;
    /* If to_ip/to_port are provided, connect first */
    if (to_ip != 0 && to_port != 0) {
        sctp_connect(fd, to_ip, to_port);
    }
    return sctp_send(fd, data, len, stream_id);
}

/* ── Implement: sctp_sendpage ────────────────── */
int sctp_sendpage(int fd, const void *page, uint16_t offset, uint16_t len, uint16_t stream_id, int flags)
{
    (void)offset;
    (void)flags;
    /* Send a page-aligned buffer (offset into page) */
    if (!page || len == 0) return -EINVAL;
    return sctp_send(fd, (const uint8_t *)page + offset, len, stream_id);
}

/* ── Implement: sctp_ioctl ────────────────── */
int sctp_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd;
    (void)request;
    (void)arg;
    kprintf("[sctp] sctp_ioctl: unsupported request %lu\n", request);
    return -EOPNOTSUPP;
}

/* ── Implement: sctp_setsockopt ────────────────── */
int sctp_setsockopt(int fd, int level, int optname, const void *optval, uint16_t optlen)
{
    if (!sctp_initialized) return -ENOSYS;
    if (level != IPPROTO_SCTP) return -ENOPROTOOPT;
    if (!optval) return -EINVAL;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    int ret = 0;
    switch (optname) {
    case 0x01: /* SCTP_RTOINFO — RTO info */
    case 0x02: /* SCTP_ASSOCINFO — assoc info */
    case 0x03: /* SCTP_INITMSG — init message params */
    case 0x04: /* SCTP_NODELAY — disable Nagle */
    case 0x05: /* SCTP_AUTOCLOSE */
    case 0x06: /* SCTP_SET_PEER_PRIMARY_ADDR */
        break; /* Accept silently */
    case 0x07: /* SCTP_PRIMARY_ADDR */
        if (optlen >= sizeof(uint32_t)) {
            uint32_t addr = *(const uint32_t *)optval;
            if (addr != 0) {
                /* Try to switch primary via transport system */
                int tp_ret = sctp_transport_set_primary(a, addr);
                if (tp_ret == 0) {
                    a->peer_ip = addr;
                } else {
                    /* If not in transport list, add it and promote */
                    sctp_transport_add(a, addr, a->peer_port);
                    sctp_transport_set_primary(a, addr);
                    a->peer_ip = addr;
                }
            }
        }
        break;
    case 0x08: /* SCTP_ADAPTATION_LAYER */
    case 0x09: /* SCTP_DISABLE_FRAGMENTS */
    case 0x0A: /* SCTP_PEER_ADDR_PARAMS */
        break;
    default:
        ret = -ENOPROTOOPT;
        break;
    }

    spinlock_release(&sctp_lock);
    return ret;
}

/* ── Implement: sctp_getsockopt ────────────────── */
int sctp_getsockopt(int fd, int level, int optname, void *optval, uint16_t *optlen)
{
    if (!sctp_initialized) return -ENOSYS;
    if (level != IPPROTO_SCTP) return -ENOPROTOOPT;
    if (!optval || !optlen) return -EINVAL;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    int ret = 0;
    switch (optname) {
    case 0x01: /* SCTP_RTOINFO */
    case 0x02: /* SCTP_ASSOCINFO */
    case 0x03: /* SCTP_INITMSG */
    case 0x04: /* SCTP_NODELAY */
        if (*optlen >= sizeof(int)) {
            *(int *)optval = 1; /* enabled */
            *optlen = sizeof(int);
        }
        break;
    case 0x09: /* SCTP_DISABLE_FRAGMENTS */
        if (*optlen >= sizeof(int)) {
            *(int *)optval = 0;
            *optlen = sizeof(int);
        }
        break;
    default:
        ret = -ENOPROTOOPT;
        break;
    }

    spinlock_release(&sctp_lock);
    return ret;
}

/* ── Implement: sctp_shutdown ────────────────── */
int sctp_shutdown(int fd, int how)
{
    if (!sctp_initialized) return -ENOSYS;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    /* how: 0=read, 1=write, 2=both */
    if (how == 1 || how == 2) {
        /* Initiate graceful shutdown via state machine */
        int ret = sctp_sm_start_shutdown(a);
        if (ret < 0) {
            spinlock_release(&sctp_lock);
            return ret;
        }
    }
    if (how == 0 || how == 2) {
    	a->rcvlen = 0; /* Discard pending data */
    	/* Also clear per-stream ready state */
    	for (int i = 0; i < SCTP_MAX_STREAMS; i++) {
    		a->in_streams[i].ready = 0;
    		a->in_streams[i].ready_len = 0;
    		a->in_streams[i].frag_active = 0;
    	}
    }

    spinlock_release(&sctp_lock);
    return 0;
}

/* ── Implement: sctp_bindx ────────────────── */
int sctp_bindx(int fd, uint32_t *addrs, uint16_t port, int addrcnt, int flags)
{
    (void)addrcnt;
    (void)flags;
    if (!addrs) return -EINVAL;

    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    /* Bind to the first address in the list */
    a->local_port = port;
    kprintf("[sctp] sctp_bindx: bound to port %u\n", port);

    spinlock_release(&sctp_lock);
    return 0;
}

/* ── Implement: sctp_peeloff ────────────────── */
int sctp_peeloff(int fd, uint32_t assoc_id)
{
    (void)assoc_id;
    /* Peel off a sub-association — not supported, return the same fd */
    kprintf("[sctp] sctp_peeloff: not supported, returning same fd %d\n", fd);
    return fd;
}

EXPORT_SYMBOL(sctp_recvmsg);
EXPORT_SYMBOL(sctp_sendmsg);
EXPORT_SYMBOL(sctp_sendpage);
EXPORT_SYMBOL(sctp_ioctl);
EXPORT_SYMBOL(sctp_setsockopt);
EXPORT_SYMBOL(sctp_getsockopt);
EXPORT_SYMBOL(sctp_shutdown);
EXPORT_SYMBOL(sctp_bindx);
EXPORT_SYMBOL(sctp_peeloff);
EXPORT_SYMBOL(handle_sctp);
/* ── Transport Management: Multi-Homing (RFC 4960 §6.4) ─────────────── */

/* Find transport index by IP address. Returns -1 if not found. */
int sctp_transport_find(const struct sctp_assoc *a, uint32_t ip)
{
    if (!a)
        return -1;
    for (uint8_t i = 0; i < a->num_transports; i++) {
        if (a->transports[i].addr == ip)
            return (int)i;
    }
    return -1;
}

/* Add a new transport address (or update if already present). */
int sctp_transport_add(struct sctp_assoc *a, uint32_t ip, uint16_t port)
{
    if (!a)
        return -EINVAL;

    /* Check if already exists */
    int idx = sctp_transport_find(a, ip);
    if (idx >= 0) {
        /* Update port and re-activate */
        a->transports[idx].port = port;
        a->transports[idx].active = 1;
        a->transports[idx].last_used = timer_get_ticks();
        kprintf("sctp: transport update for " NIPQUAD_FMT ": %s\n",
                NIPQUAD(ip), a->transports[idx].primary ? "primary" : "secondary");
        return 0;
    }

    /* Add new transport */
    if (a->num_transports >= SCTP_MAX_TRANSPORTS) {
        kprintf("sctp: max transports reached, cannot add " NIPQUAD_FMT "\n",
                NIPQUAD(ip));
        return -ENOSPC;
    }

    uint8_t slot = a->num_transports;
    a->transports[slot].addr = ip;
    a->transports[slot].port = port;
    a->transports[slot].primary = (a->num_transports == 0) ? 1 : 0;
    a->transports[slot].active = 1;
    a->transports[slot].rtt = 0;
    a->transports[slot].rto = 3000;  /* 3 seconds default RTO */
    a->transports[slot].last_used = timer_get_ticks();
    a->num_transports++;

    /* If this is the first transport, set it as primary */
    if (a->transports[slot].primary) {
        a->primary_path = slot;
    }

    kprintf("sctp: transport added " NIPQUAD_FMT ": port=%u %s (total=%u)\n",
            NIPQUAD(ip), port,
            a->transports[slot].primary ? "primary" : "secondary",
            a->num_transports);
    return 0;
}

/* Set the primary path to the given IP address. */
int sctp_transport_set_primary(struct sctp_assoc *a, uint32_t ip)
{
    if (!a)
        return -EINVAL;

    int idx = sctp_transport_find(a, ip);
    if (idx < 0) {
        kprintf("sctp: cannot set primary — " NIPQUAD_FMT " not in transport list\n",
                NIPQUAD(ip));
        return -EINVAL;
    }

    /* Clear primary flag on all transports */
    for (uint8_t i = 0; i < a->num_transports; i++)
        a->transports[i].primary = 0;

    /* Set new primary */
    a->transports[idx].primary = 1;
    a->primary_path = (uint8_t)idx;

    kprintf("sctp: primary path switched to " NIPQUAD_FMT ": port=%u\n",
            NIPQUAD(ip), a->transports[idx].port);
    return 0;
}

/* Remove all transports (called on assoc close/reset). */
void sctp_transport_remove_all(struct sctp_assoc *a)
{
    if (!a)
        return;
    memset(a->transports, 0, sizeof(a->transports));
    a->num_transports = 0;
    a->primary_path = 0;
}

/* ════════════════════════════════════════════════════════════════════════
 * SCTP HEARTBEAT — Reachability Detection (RFC 4960 §8.3)
 * ════════════════════════════════════════════════════════════════════════
 *
 * HEARTBEAT is used to verify reachability of a destination transport
 * address. A HEARTBEAT chunk carries a Heartbeat Info parameter (type=1)
 * containing the sender's timestamp. The receiver echoes it back in a
 * HEARTBEAT-ACK, allowing RTT measurement.
 *
 * Path failure: SCTP_HB_PROBES_MAX (4) consecutive unanswered HEARTBEATs
 * cause the path to be marked inactive.
 *
 * Path recovery: Receiving a HEARTBEAT-ACK on an inactive path marks it
 * active again.
 */

/* ── Initialize heartbeat fields for an association ────────────────────── */
void sctp_heartbeat_init(struct sctp_assoc *a)
{
    if (!a)
        return;
    a->hb_last_check = timer_get_ticks();
    a->hb_interval = SCTP_HB_INTERVAL_DEFAULT;
    for (uint8_t i = 0; i < a->num_transports; i++) {
        a->transports[i].hb_last_sent = 0;
        a->transports[i].hb_timeout_count = 0;
        a->transports[i].hb_pending = 0;
    }
}

/* ── Send a HEARTBEAT chunk on a specific transport ──────────────────────
 *
 * Builds and sends a HEARTBEAT chunk (RFC 4960 §3.3.5):
 *   Type=4, Flags=0, Length=16 (4 chunk hdr + 4 param hdr + 4 timestamp)
 *   Heartbeat Info parameter (Type=1, Length=8): sender's timestamp
 */
int sctp_heartbeat_send(struct sctp_assoc *a, struct sctp_transport *tp)
{
    if (!a || !tp)
        return -EINVAL;

    /* HEARTBEAT chunk layout:
     *   sctp_header (12)
     *   sctp_chunk  (4)  type=4, len=16
     *   param_type  (2)  type=1 (HB INFO)
     *   param_len   (2)  len=8
     *   timestamp   (4)  sender's time
     */
    uint8_t pkt[sizeof(struct sctp_header) + 16];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    memset(sh, 0, sizeof(*sh));
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(tp->port);
    sh->vtag = htonl(a->peer_tag);
    sh->checksum = 0;

    struct sctp_chunk *chunk = (struct sctp_chunk *)(pkt + sizeof(*sh));
    chunk->type = SCTP_HEARTBEAT;
    chunk->flags = 0;
    chunk->length = htons(16);

    /* Heartbeat Info parameter (Type=1, Length=8) */
    uint8_t *param = (uint8_t *)(chunk + 1);
    param[0] = 0;                    /* Param type high byte */
    param[1] = SCTP_PARAM_HB_INFO;  /* Param type low byte = 1 */
    param[2] = 0;                    /* Param length high byte */
    param[3] = 8;                    /* Param length low byte = 8 */

    /* Sender's timestamp (4 bytes) — used to calculate RTT on ACK */
    uint64_t now = timer_get_ticks();
    uint32_t timestamp = (uint32_t)now;
    param[4] = (uint8_t)(timestamp >> 24);
    param[5] = (uint8_t)(timestamp >> 16);
    param[6] = (uint8_t)(timestamp >> 8);
    param[7] = (uint8_t)(timestamp);

    uint16_t pkt_len = sizeof(*sh) + 16;
    send_ip(tp->addr, IPPROTO_SCTP, pkt, pkt_len);
    a->tx_packets++;

    /* Record when we sent this heartbeat */
    tp->hb_last_sent = timestamp;
    tp->hb_pending = 1;

    kprintf("sctp: HEARTBEAT sent to " NIPQUAD_FMT ":%u ts=%u\n",
            NIPQUAD(tp->addr), tp->port, timestamp);
    return 0;
}

/* ── Check all transports and send HEARTBEAT if due ──────────────────────
 *
 * Called periodically from handle_sctp, sctp_send, sctp_recv while
 * holding the SCTP lock. Sends HEARTBEAT to transports that have been
 * idle longer than hb_interval and don't already have an outstanding HB.
 *
 * On path failure (too many consecutive missed heartbeats), marks the
 * transport inactive and may trigger path switchover.
 */
void sctp_heartbeat_check(struct sctp_assoc *a)
{
    if (!a)
        return;

    uint64_t now = timer_get_ticks();
    uint32_t now32 = (uint32_t)now;

    /* Only check once per association per hb_interval/4 ticks minimum */
    uint32_t min_check_interval = a->hb_interval / 4;
    if (min_check_interval < 50)
        min_check_interval = 50;  /* Minimum 500ms between checks */
    if (now32 - (uint32_t)a->hb_last_check < min_check_interval)
        return;

    a->hb_last_check = now;

    for (uint8_t i = 0; i < a->num_transports; i++) {
        struct sctp_transport *tp = &a->transports[i];

        /* ── Check for HEARTBEAT timeout ────────────────────────────── */
        if (tp->hb_pending) {
            uint32_t elapsed = now32 - tp->hb_last_sent;
            if (elapsed >= tp->rto) {
                /* HEARTBEAT timed out */
                tp->hb_pending = 0;
                tp->hb_timeout_count++;

                kprintf("sctp: HEARTBEAT timeout on " NIPQUAD_FMT ":%u "
                        "(timeout=%u/%u)\n",
                        NIPQUAD(tp->addr), tp->port,
                        tp->hb_timeout_count, SCTP_HB_PROBES_MAX);

                if (tp->hb_timeout_count >= SCTP_HB_PROBES_MAX) {
                    /* Path failure — mark transport inactive */
                    tp->active = 0;
                    kprintf("sctp: path " NIPQUAD_FMT ":%u INACTIVE "
                            "(%u consecutive HB timeouts)\n",
                            NIPQUAD(tp->addr), tp->port,
                            tp->hb_timeout_count);

                    /* If this was the primary path, try to switch */
                    if (i == a->primary_path) {
                        for (uint8_t j = 0; j < a->num_transports; j++) {
                            if (j != i && a->transports[j].active) {
                                sctp_transport_set_primary(a,
                                    a->transports[j].addr);
                                kprintf("sctp: switched primary from "
                                        NIPQUAD_FMT " to " NIPQUAD_FMT "\n",
                                        NIPQUAD(tp->addr),
                                        NIPQUAD(a->transports[j].addr));
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* ── Send HEARTBEAT if transport is idle and due ────────────── */
        uint32_t idle = now32 - (uint32_t)tp->last_used;
        if (idle >= a->hb_interval && !tp->hb_pending && tp->active) {
            sctp_heartbeat_send(a, tp);
        }
    }
}

EXPORT_SYMBOL(sctp_heartbeat_init);
EXPORT_SYMBOL(sctp_heartbeat_send);
EXPORT_SYMBOL(sctp_heartbeat_check);

/* ── ASCONF / ASCONF-ACK Handling (RFC 5061) ────────────────────────── */

/* ASCONF parameter types (RFC 5061 §4.2) */
#define SCTP_PARAM_ADD_IP       0xC001  /* Add IP address */
#define SCTP_PARAM_DEL_IP       0xC002  /* Delete IP address */
#define SCTP_PARAM_SET_PRIMARY  0xC004  /* Set primary address */
#define SCTP_PARAM_SUCCESS_CAUSE 0xC005 /* Success indication */
#define SCTP_PARAM_ERR_CAUSE    0xC006  /* Error cause */

/* Send an ASCONF-ACK chunk in response to an ASCONF. */
static int sctp_send_asconf_ack(struct sctp_assoc *a, uint32_t src_ip,
                                uint32_t serial, uint16_t num_params)
{
    if (!a)
        return -EINVAL;

    uint16_t result_list_size = num_params * 8;
    uint16_t chunk_data_len = 4 + result_list_size;
    uint16_t chunk_total = sizeof(struct sctp_chunk) + chunk_data_len;

    uint8_t pkt[sizeof(struct sctp_header) + 256];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    memset(sh, 0, sizeof(*sh));
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(a->transports[0].port);
    sh->vtag = htonl(a->peer_tag);

    struct sctp_chunk *chk = (struct sctp_chunk *)(pkt + sizeof(*sh));
    chk->type = SCTP_ASCONF_ACK;
    chk->flags = 0;
    chk->length = htons(chunk_total);

    uint32_t *serial_field = (uint32_t *)(chk + 1);
    *serial_field = htonl(serial);

    uint8_t *results = (uint8_t *)(serial_field + 1);
    for (uint16_t i = 0; i < num_params; i++) {
        uint16_t *result_hdr = (uint16_t *)(results + i * 8);
        result_hdr[0] = htons(SCTP_PARAM_SUCCESS_CAUSE);
        result_hdr[1] = htons(8);
    }

    uint16_t pkt_len = sizeof(*sh) + chunk_total;
    uint32_t dst_ip = a->transports[a->primary_path].addr;
    send_ip(dst_ip, IPPROTO_SCTP, pkt, pkt_len);
    a->tx_packets++;

    kprintf("sctp: ASCONF-ACK sent to " NIPQUAD_FMT " (serial=%u, %u params)\n",
            NIPQUAD(dst_ip), serial, num_params);
    return 0;
}

/* Handle incoming ASCONF chunk (RFC 5061 §4.1). */
int sctp_handle_asconf(struct sctp_assoc *a, uint32_t src_ip,
                        const struct sctp_chunk *chunk, uint16_t chunk_len)
{
    if (!a || !chunk)
        return -EINVAL;
    if (chunk_len < sizeof(*chunk) + 8)
        return -EINVAL;

    const uint8_t *data = (const uint8_t *)(chunk + 1);
    uint16_t data_len = chunk_len - sizeof(*chunk);

    uint32_t serial = ntohl(*(const uint32_t *)data);
    uint16_t remaining = data_len - 4;
    const uint8_t *ptr = data + 4;
    uint16_t num_params = 0;

    /* Count parameters for the ACK */
    uint16_t count_remaining = remaining;
    const uint8_t *count_ptr = ptr;
    while (count_remaining >= 8) {
        uint16_t ptype = ntohs(*(const uint16_t *)count_ptr);
        uint16_t plen  = ntohs(*(const uint16_t *)(count_ptr + 2));
        if (plen < 8 || plen > count_remaining)
            break;
        if (ptype < 0xC000) {
            count_ptr += plen;
            count_remaining -= plen;
            continue;
        }
        num_params++;
        count_ptr += plen;
        count_remaining -= plen;
    }

    kprintf("sctp: ASCONF from " NIPQUAD_FMT " serial=%u params=%u\n",
            NIPQUAD(src_ip), serial, num_params);

    /* Process each ASCONF parameter */
    remaining = data_len - 4;
    ptr = data + 4;

    while (remaining >= 8) {
        uint16_t ptype = ntohs(*(const uint16_t *)ptr);
        uint16_t plen  = ntohs(*(const uint16_t *)(ptr + 2));

        if (plen < 8 || plen > remaining)
            break;

        if (ptype < 0xC000) {
            ptr += plen;
            remaining -= plen;
            continue;
        }

        if (plen < 12) {
            ptr += plen;
            remaining -= plen;
            continue;
        }

        uint32_t param_addr = ntohl(*(const uint32_t *)(ptr + 4));

        switch (ptype) {
        case SCTP_PARAM_ADD_IP:
            kprintf("sctp: ASCONF ADD_IP " NIPQUAD_FMT "\n",
                    NIPQUAD(param_addr));
            sctp_transport_add(a, param_addr, a->peer_port);
            break;

        case SCTP_PARAM_DEL_IP:
            kprintf("sctp: ASCONF DEL_IP " NIPQUAD_FMT "\n",
                    NIPQUAD(param_addr));
            {
                int idx = sctp_transport_find(a, param_addr);
                if (idx >= 0)
                    a->transports[idx].active = 0;
            }
            break;

        case SCTP_PARAM_SET_PRIMARY:
            kprintf("sctp: ASCONF SET_PRIMARY " NIPQUAD_FMT "\n",
                    NIPQUAD(param_addr));
            sctp_transport_set_primary(a, param_addr);
            break;

        default:
            kprintf("sctp: ASCONF unknown param type 0x%04x\n", ptype);
            break;
        }

        ptr += plen;
        remaining -= plen;
    }

    return sctp_send_asconf_ack(a, src_ip, serial, num_params);
}

EXPORT_SYMBOL(sctp_transport_find);
EXPORT_SYMBOL(sctp_transport_add);
EXPORT_SYMBOL(sctp_transport_set_primary);
EXPORT_SYMBOL(sctp_transport_remove_all);
EXPORT_SYMBOL(sctp_handle_asconf);

#include "module.h"

/* Built-in kernel init: register via initcall system */
#ifndef MODULE
device_initcall(sctp_init);
#endif

/* Loadable module entry/exit */
#ifdef MODULE
int __init init_module(void)
{
    sctp_init();
    kprintf("[sctp] module loaded\n");
    return 0;
}

void __exit cleanup_module(void)
{
    sctp_initialized = 0;
    kprintf("[sctp] module unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("SCTP: Stream Control Transmission Protocol (RFC 4960)");
MODULE_VERSION("1.0");
#endif /* MODULE */
