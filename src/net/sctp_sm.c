/* sctp_sm.c — SCTP State Machine (RFC 4960)
 *
 * Handles the INIT / INIT-ACK / COOKIE-ECHO / COOKIE-ACK
 * association-setup four-way handshake.
 */

#include "sctp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "crc.h"
#include "timer.h"
#include "errno.h"
#include "export.h"

/* ── Cookie secret (rotated periodically in a full implementation) ─── */
static uint8_t cookie_secret[SCTP_COOKIE_SECRET_SIZE];
static int     cookie_secret_initialized = 0;

static void ensure_cookie_secret(void)
{
    if (cookie_secret_initialized)
        return;
    /* Derive a simple secret from the current tick count — a real
     * implementation would use a proper RNG and periodic rotation. */
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < SCTP_COOKIE_SECRET_SIZE; i++) {
        cookie_secret[i] = (uint8_t)((now >> (8 * (i % 8))) ^ (uint8_t)(i * 37));
    }
    cookie_secret_initialized = 1;
}

/* ── Cookie integrity: CRC32 over cookie data XORed with secret ─────── */
static uint32_t cookie_integrity(const struct sctp_cookie *c,
                                 const uint8_t *secret)
{
    /* CRC32 of (cookie ^ secret) up to the crc field */
    uint32_t crc = 0xFFFFFFFFU;
    const uint8_t *data = (const uint8_t *)c;
    const uint8_t *sec = secret;
    uint32_t len = (uint32_t)(&c->crc - &c->local_tag);
    for (uint32_t i = 0; i < len; i++) {
        crc = crc32(crc, &(uint8_t){data[i] ^ sec[i % SCTP_COOKIE_SECRET_SIZE]}, 1);
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ── Cookie generation (RFC 4960 §5.1.3) ────────────────────────────── */
int sctp_cookie_generate(const struct sctp_cookie *cookie_in,
                          uint8_t *out_cookie, uint16_t *out_len)
{
    if (!cookie_in || !out_cookie || !out_len)
        return -EINVAL;
    if (*out_len < sizeof(struct sctp_cookie))
        return -ENOSPC;

    ensure_cookie_secret();

    struct sctp_cookie *c = (struct sctp_cookie *)out_cookie;
    memcpy(c, cookie_in, sizeof(*c));
    c->crc = 0;
    c->crc = htonl(cookie_integrity(c, cookie_secret));
    *out_len = sizeof(struct sctp_cookie);
    return 0;
}

/* ── Cookie validation ──────────────────────────────────────────────── */
int sctp_cookie_validate(const uint8_t *cookie_data, uint16_t cookie_len,
                          struct sctp_cookie *cookie_out)
{
    if (!cookie_data || !cookie_out)
        return -EINVAL;
    if (cookie_len < sizeof(struct sctp_cookie))
        return -EINVAL;

    ensure_cookie_secret();

    struct sctp_cookie tmp;
    memcpy(&tmp, cookie_data, sizeof(tmp));

    /* Restore CRC field to zero for verification */
    uint32_t stored_crc = ntohl(tmp.crc);
    tmp.crc = 0;

    uint32_t expected = cookie_integrity(&tmp, cookie_secret);
    if (stored_crc != expected)
        return -EINVAL;

    /* Check for staleness (30-second TTL at 100 ticks/sec = 3000 ticks) */
    uint64_t now = timer_get_ticks();
    uint32_t cookie_time = ntohl(tmp.timestamp);
    if (now > (uint64_t)cookie_time + 3000)
        return -ETIME;  /* Cookie expired */

    memcpy(cookie_out, &tmp, sizeof(tmp));
    return 0;
}

/* ── Build and send an INIT-ACK chunk ──────────────────────────────────
 *
 * Called when a passive-side TCB receives an INIT chunk.
 * The a->peer_tag and a->peer_ip must already be set.
 */
int sctp_sm_send_init_ack(struct sctp_assoc *a, uint32_t peer_ip,
                           uint16_t peer_port, uint32_t peer_tag,
                           uint32_t peer_tsn, uint8_t num_in,
                           uint8_t num_out)
{
    /* Prepare cookie */
    struct sctp_cookie cookie_data;
    memset(&cookie_data, 0, sizeof(cookie_data));
    cookie_data.local_tag    = htonl(a->local_tag);
    cookie_data.peer_tag     = htonl(peer_tag);
    cookie_data.local_tsn    = htonl(100);  /* Our initial TSN */
    cookie_data.peer_tsn     = htonl(peer_tsn);
    cookie_data.local_port   = htons(a->local_port);
    cookie_data.peer_port    = htons(peer_port);
    cookie_data.peer_ip      = htonl(peer_ip);
    cookie_data.num_in_streams  = htons((a->num_in_streams > num_in) ? a->num_in_streams : num_in);
    cookie_data.num_out_streams = htons((a->num_out_streams > num_out) ? a->num_out_streams : num_out);
    cookie_data.timestamp    = htonl((uint32_t)timer_get_ticks());

    uint8_t cookie_buf[SCTP_MAX_COOKIE_SIZE];
    uint16_t cookie_len = sizeof(cookie_buf);
    int ret = sctp_cookie_generate(&cookie_data, cookie_buf, &cookie_len);
    if (ret < 0)
        return ret;

    /* Pad cookie to 4-byte boundary */
    uint16_t cookie_pad = (4 - (cookie_len % 4)) % 4;

    /* Build INIT-ACK chunk
     * Fixed params: initiate_tag (4), a_rwnd (4),
     *               num_out_streams (2), num_in_streams (2),
     *               initial_tsn (4) = 16 bytes
     * Then optional params: state cookie + padding
     */
    uint16_t fixed_params = 16;
    uint16_t param_total  = fixed_params + 4 + cookie_len + cookie_pad;
    uint16_t chunk_len    = sizeof(struct sctp_chunk) + param_total;

    /* Build SCTP common header + INIT-ACK chunk */
    uint8_t pkt[sizeof(struct sctp_header) + sizeof(struct sctp_chunk) +
                SCTP_MAX_COOKIE_SIZE + 32];
    struct sctp_header *sh = (struct sctp_header *)pkt;
    memset(sh, 0, sizeof(*sh));
    sh->src_port = htons(a->local_port);
    sh->dst_port = htons(peer_port);
    sh->vtag = htonl(peer_tag);  /* Tag = peer's tag for INIT-ACK */

    struct sctp_chunk *chunk = (struct sctp_chunk *)(pkt + sizeof(*sh));
    chunk->type  = SCTP_INIT_ACK;
    chunk->flags = 0;
    chunk->length = htons(chunk_len);

    /* Fixed INIT-ACK parameters */
    uint8_t *params = (uint8_t *)(chunk + 1);
    uint32_t *fixed = (uint32_t *)params;
    fixed[0] = htonl(a->local_tag);  /* Initiate tag */
    fixed[1] = htonl(65536);         /* a_rwnd */
    fixed[2] = (uint32_t)htons(a->num_out_streams) |
               ((uint32_t)htons(a->num_in_streams) << 16);
    fixed[3] = htonl(100);           /* Initial TSN */

    /* State cookie parameter (type=7, RFC 4960 §3.3.1) */
    uint8_t *cookie_param = params + fixed_params;
    cookie_param[0] = 0;              /* Param type high */
    cookie_param[1] = 7;              /* Param type = 7 (State Cookie) */
    cookie_param[2] = (uint8_t)((4 + cookie_len + cookie_pad) >> 8);
    cookie_param[3] = (uint8_t)(4 + cookie_len + cookie_pad);
    memcpy(cookie_param + 4, cookie_buf, cookie_len);
    /* Zero-pad to 4 bytes */
    if (cookie_pad > 0)
        memset(cookie_param + 4 + cookie_len, 0, cookie_pad);

    sh->checksum = 0;  /* CRC32c would go here */

    uint16_t pkt_len = sizeof(*sh) + chunk_len;
    send_ip(peer_ip, IPPROTO_SCTP, pkt, pkt_len);

    kprintf("sctp: sent INIT-ACK to " NIPQUAD_FMT ":%u (tag=0x%08x)\n",
            NIPQUAD(peer_ip), ntohs(sh->dst_port),
            a->local_tag);
    return 0;
}

/* ── Handle incoming INIT (RFC 4960 §5.1) ─────────────────────────────
 *
 * When we (as the responder) receive an INIT, we:
 * 1. Parse the init params (peer tag, TSN, stream info)
 * 2. Create a temporary TCB
 * 3. Generate a state cookie
 * 4. Send INIT-ACK
 */
int sctp_sm_handle_init(struct sctp_assoc *a, uint32_t src_ip,
                         const struct sctp_header *sh,
                         const struct sctp_chunk *chunk, uint16_t chunk_len)
{
    (void)chunk;
    (void)chunk_len;

    /* Extract fixed INIT parameters (12 bytes after chunk header) */
    const uint8_t *params = (const uint8_t *)(chunk + 1);
    uint32_t peer_tag  = ntohl(*(const uint32_t *)(params + 0));
    /* uint32_t a_rwnd = ntohl(*(const uint32_t *)(params + 4)); */
    uint16_t peer_num_out = ntohs(*(const uint16_t *)(params + 8));
    uint16_t peer_num_in  = ntohs(*(const uint16_t *)(params + 10));
    uint32_t peer_tsn     = ntohl(*(const uint32_t *)(params + 12));

    /* Record peer info */
    a->peer_ip   = src_ip;
    a->peer_port = ntohs(sh->src_port);
    a->peer_tag  = peer_tag;

    /* Register source address as a transport for multi-homing */
    sctp_transport_add(a, src_ip, ntohs(sh->src_port));

    a->num_in_streams  = (uint8_t)((peer_num_in  < SCTP_MAX_STREAMS) ? peer_num_in  : SCTP_MAX_STREAMS);
    a->num_out_streams = (uint8_t)((peer_num_out < SCTP_MAX_STREAMS) ? peer_num_out : SCTP_MAX_STREAMS);

    /* Initialize TSN tracking */
    /* a->initial_tsn = 100; — would be stored in assoc */

    a->rx_packets++;
    a->state = SCTP_STATE_COOKIE_WAIT;  /* Temp state until COOKIE-ECHO */

    kprintf("sctp: INIT from " NIPQUAD_FMT ":%u tag=0x%08x "
            "streams=%u/%u tsn=%u\n",
            NIPQUAD(src_ip), ntohs(sh->src_port), peer_tag,
            a->num_in_streams, a->num_out_streams, peer_tsn);

    /* Send INIT-ACK */
    int ret = sctp_sm_send_init_ack(a, src_ip, ntohs(sh->src_port),
                                     peer_tag, peer_tsn,
                                     a->num_in_streams, a->num_out_streams);
    return ret;
}

/* ── Handle incoming INIT-ACK ──────────────────────────────────────────
 *
 * When we (the initiator) receive an INIT-ACK, we:
 * 1. Extract fixed params (peer's tag, TSN, streams)
 * 2. Extract the state cookie
 * 3. Send COOKIE-ECHO containing the cookie
 */
int sctp_sm_handle_init_ack(struct sctp_assoc *a, uint32_t src_ip,
                             const struct sctp_header *sh,
                             const struct sctp_chunk *chunk,
                             uint16_t chunk_len)
{
    const uint8_t *params = (const uint8_t *)(chunk + 1);
    uint16_t param_remaining = chunk_len - sizeof(*chunk);
    if (param_remaining < 16) {
        kprintf("sctp: INIT-ACK too short (%u)\n", param_remaining);
        return -EINVAL;
    }

    uint32_t peer_tag  = ntohl(*(const uint32_t *)(params + 0));
    /* uint32_t a_rwnd = ntohl(*(const uint32_t *)(params + 4)); */
    uint16_t peer_num_out = ntohs(*(const uint16_t *)(params + 8));
    uint16_t peer_num_in  = ntohs(*(const uint16_t *)(params + 10));
    uint32_t peer_tsn     = ntohl(*(const uint32_t *)(params + 12));

    /* Update peer info */
    a->peer_ip   = src_ip;
    a->peer_port = ntohs(sh->src_port);
    a->peer_tag  = peer_tag;

    /* Register source address as a transport for multi-homing */
    sctp_transport_add(a, src_ip, ntohs(sh->src_port));

    a->num_in_streams  = (uint8_t)((peer_num_in  < SCTP_MAX_STREAMS) ? peer_num_in  : SCTP_MAX_STREAMS);
    a->num_out_streams = (uint8_t)((peer_num_out < SCTP_MAX_STREAMS) ? peer_num_out : SCTP_MAX_STREAMS);

    /* Parse optional parameters to find state cookie */
    uint16_t off = 16;
    const uint8_t *cookie_data = NULL;
    uint16_t cookie_len = 0;

    while (off + 4 <= param_remaining) {
        uint16_t ptype  = ((uint16_t)params[off] << 8) | params[off + 1];
        uint16_t plen   = ((uint16_t)params[off + 2] << 8) | params[off + 3];

        if (plen < 4 || off + plen > param_remaining)
            break;

        if (ptype == 7) { /* State Cookie parameter */
            cookie_data = params + off + 4;
            cookie_len  = plen - 4;
            break;
        }
        off += plen;
    }

    if (!cookie_data) {
        kprintf("sctp: INIT-ACK missing state cookie\n");
        return -EINVAL;
    }

    /* Validate the cookie */
    struct sctp_cookie cookie_out;
    int ret = sctp_cookie_validate(cookie_data, cookie_len, &cookie_out);
    if (ret < 0) {
        kprintf("sctp: INIT-ACK cookie invalid (ret=%d)\n", ret);
        return ret;
    }

    a->rx_packets++;

    kprintf("sctp: INIT-ACK from " NIPQUAD_FMT ":%u tag=0x%08x "
            "streams=%u/%u cookie=%uB\n",
            NIPQUAD(src_ip), ntohs(sh->src_port), peer_tag,
            a->num_in_streams, a->num_out_streams, cookie_len);

    /* Send COOKIE-ECHO */
    {
        uint8_t ce_pkt[sizeof(struct sctp_header) + sizeof(struct sctp_chunk) +
                       SCTP_MAX_COOKIE_SIZE];
        struct sctp_header *ce_sh = (struct sctp_header *)ce_pkt;
        memset(ce_sh, 0, sizeof(*ce_sh));
        ce_sh->src_port = htons(a->local_port);
        ce_sh->dst_port = htons(a->peer_port);
        ce_sh->vtag = htonl(peer_tag);

        struct sctp_chunk *ce_chunk = (struct sctp_chunk *)(ce_pkt + sizeof(*ce_sh));
        ce_chunk->type  = SCTP_COOKIE_ECHO;
        ce_chunk->flags = 0;
        ce_chunk->length = htons(sizeof(*ce_chunk) + cookie_len);
        memcpy(ce_pkt + sizeof(*ce_sh) + sizeof(*ce_chunk),
               cookie_data, cookie_len);

        uint16_t ce_pkt_len = sizeof(*ce_sh) + sizeof(*ce_chunk) + cookie_len;
        send_ip(a->peer_ip, IPPROTO_SCTP, ce_pkt, ce_pkt_len);
        a->tx_packets++;
        a->state = SCTP_STATE_COOKIE_ECHOED;

        kprintf("sctp: sent COOKIE-ECHO to " NIPQUAD_FMT ":%u\n",
                NIPQUAD(a->peer_ip), a->peer_port);
    }

    return 0;
}

/* ── Handle incoming COOKIE-ECHO (RFC 4960 §5.1.3) ─────────────────────
 *
 * When we receive a COOKIE-ECHO:
 * 1. Validate the cookie
 * 2. Recreate the TCB
 * 3. Send COOKIE-ACK
 * 4. Transition to ESTABLISHED
 */
int sctp_sm_handle_cookie_echo(struct sctp_assoc *a, uint32_t src_ip,
                                const struct sctp_header *sh,
                                const struct sctp_chunk *chunk,
                                uint16_t chunk_len)
{
    uint16_t cookie_len = chunk_len - sizeof(*chunk);
    const uint8_t *cookie_data = (const uint8_t *)(chunk + 1);

    if (cookie_len < sizeof(struct sctp_cookie)) {
        kprintf("sctp: COOKIE-ECHO too short (%u)\n", cookie_len);
        return -EINVAL;
    }

    struct sctp_cookie cookie_out;
    int ret = sctp_cookie_validate(cookie_data, cookie_len, &cookie_out);
    if (ret < 0) {
        kprintf("sctp: COOKIE-ECHO invalid cookie (ret=%d)\n", ret);
        return ret;
    }

    /* Recreate association from cookie */
    a->peer_ip   = src_ip;
    a->peer_port = ntohs(sh->src_port);
    a->local_tag = ntohl(cookie_out.local_tag);
    a->peer_tag  = ntohl(cookie_out.peer_tag);
    a->num_in_streams  = ntohs(cookie_out.num_in_streams);
    a->num_out_streams = ntohs(cookie_out.num_out_streams);

    /* Register source address as a transport for multi-homing */
    sctp_transport_add(a, src_ip, ntohs(sh->src_port));

    a->rx_packets++;
    a->state = SCTP_STATE_ESTABLISHED;

    kprintf("sctp: COOKIE-ECHO valid from " NIPQUAD_FMT ":%u "
            "- association established\n",
            NIPQUAD(src_ip), ntohs(sh->src_port));

    /* Send COOKIE-ACK */
    {
        uint8_t ca_pkt[sizeof(struct sctp_header) + sizeof(struct sctp_chunk)];
        struct sctp_header *ca_sh = (struct sctp_header *)ca_pkt;
        memset(ca_sh, 0, sizeof(*ca_sh));
        ca_sh->src_port = sh->dst_port;
        ca_sh->dst_port = sh->src_port;
        ca_sh->vtag = htonl(a->peer_tag);
        ca_sh->checksum = 0;

        struct sctp_chunk *ca_chunk = (struct sctp_chunk *)(ca_pkt + sizeof(*ca_sh));
        ca_chunk->type  = SCTP_COOKIE_ACK;
        ca_chunk->flags = 0;
        ca_chunk->length = htons(sizeof(*ca_chunk));

        uint16_t ca_pkt_len = sizeof(*ca_sh) + sizeof(*ca_chunk);
        send_ip(a->peer_ip, IPPROTO_SCTP, ca_pkt, ca_pkt_len);
        a->tx_packets++;

        kprintf("sctp: sent COOKIE-ACK to " NIPQUAD_FMT ":%u\n",
                NIPQUAD(a->peer_ip), a->peer_port);
    }

    return 0;
}

/* ── Handle incoming COOKIE-ACK ────────────────────────────────────────
 *
 * Final step: association is fully established.
 */
int sctp_sm_handle_cookie_ack(struct sctp_assoc *a,
                               const struct sctp_header *sh)
{
    (void)sh;
    a->state = SCTP_STATE_ESTABLISHED;
    a->rx_packets++;

    kprintf("sctp: COOKIE-ACK received - association ESTABLISHED\n");
    return 0;
}

EXPORT_SYMBOL(sctp_cookie_generate);
EXPORT_SYMBOL(sctp_cookie_validate);
EXPORT_SYMBOL(sctp_sm_send_init_ack);
EXPORT_SYMBOL(sctp_sm_handle_init);
EXPORT_SYMBOL(sctp_sm_handle_init_ack);
EXPORT_SYMBOL(sctp_sm_handle_cookie_echo);
EXPORT_SYMBOL(sctp_sm_handle_cookie_ack);
#include "module.h"
module_init(sctp_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SCTP State Machine — INIT/INIT-ACK/COOKIE handshake");
MODULE_AUTHOR("OS Kernel Team");
