/* sctp.c — Stream Control Transmission Protocol (RFC 4960) */

#include "sctp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

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
    (void)stream_id;
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a || a->state != SCTP_STATE_ESTABLISHED) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

    /* Build DATA chunk */
    uint8_t pkt[sizeof(struct sctp_header) + sizeof(struct sctp_chunk) + 65536];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(a->peer_port);
    sh->vtag = htonl(a->peer_tag);

    struct sctp_chunk *chunk = (struct sctp_chunk *)(pkt + sizeof(*sh));
    chunk->type = SCTP_DATA;
    chunk->flags = 0x07;  /* B=1, E=1, U=1 (unordered complete) */
    chunk->length = htons(sizeof(*chunk) + len);
    memcpy(pkt + sizeof(*sh) + sizeof(*chunk), data, len);

    uint16_t pkt_len = sizeof(*sh) + sizeof(*chunk) + len;
    sh->checksum = 0;  /* CRC32c would go here */

    send_ip(a->peer_ip, IPPROTO_SCTP, pkt, pkt_len);
    a->tx_packets++;
    spinlock_release(&sctp_lock);
    return len;
}

int sctp_recv(int fd, void *buf, uint16_t maxlen, uint16_t *stream_id)
{
    (void)stream_id;
    spinlock_acquire(&sctp_lock);
    struct sctp_assoc *a = sctp_find_by_fd(fd);
    if (!a) {
        spinlock_release(&sctp_lock);
        return -EINVAL;
    }

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

void sctp_close(int fd)
{
    spinlock_acquire(&sctp_lock);
    for (int i = 0; i < SCTP_MAX_ASSOCS; i++) {
        if (sctp_assocs[i].used && sctp_assocs[i].local_port == (uint16_t)fd) {
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
            /* Respond with INIT-ACK */
            a->peer_ip = src_ip;
            a->peer_port = ntohs(sh->src_port);
            a->peer_tag = ntohl(*(const uint32_t *)(payload + sizeof(*sh) + sizeof(*chunk)));
            a->state = SCTP_STATE_ESTABLISHED;
            break;

        case SCTP_DATA: {
            uint16_t chunk_data_len = chunk_len - sizeof(*chunk);
            size_t data_len = chunk_data_len;
            size_t rcvbuf_size = sizeof(a->rcvbuf);
            if (data_len > rcvbuf_size) data_len = rcvbuf_size;
            memcpy(a->rcvbuf, (uint8_t *)(chunk + 1), data_len);
            a->rcvlen = data_len;
            break;
        }

        case SCTP_ABORT:
            a->state = SCTP_STATE_CLOSED;
            break;

        case SCTP_SHUTDOWN:
            a->state = SCTP_STATE_SHUTDOWN_RECEIVED;
            break;
        }

        remaining -= chunk_len;
        chunk = (const struct sctp_chunk *)((const uint8_t *)chunk + chunk_len);
    }

    spinlock_release(&sctp_lock);
}

EXPORT_SYMBOL(sctp_init);
EXPORT_SYMBOL(sctp_create);
EXPORT_SYMBOL(sctp_bind);
EXPORT_SYMBOL(sctp_connect);
EXPORT_SYMBOL(sctp_send);
EXPORT_SYMBOL(sctp_recv);
EXPORT_SYMBOL(sctp_close);
EXPORT_SYMBOL(handle_sctp);
