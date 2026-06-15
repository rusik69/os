/* ipsec.c — IPsec AH/ESP protocol, tunnel and transport mode, SADB */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "errno.h"

/* IP protocol numbers for AH and ESP */
#define IP_PROTO_AH   51
#define IP_PROTO_ESP  50

/* AH header (RFC 4302) */
struct ah_header {
    uint8_t  next_hdr;
    uint8_t  payload_len;   /* in 32-bit words minus 2 */
    uint16_t reserved;
    uint32_t spi;           /* Security Parameters Index */
    uint32_t seq_no;        /* sequence number */
    /* ICV (integrity check value) follows */
} __attribute__((packed));

/* ESP header (RFC 4303) */
struct esp_header {
    uint32_t spi;
    uint32_t seq_no;
    /* payload data + padding + pad length + next header + ICV follows */
} __attribute__((packed));

/* ESP trailer (after payload, before ICV) */
struct esp_trailer {
    uint8_t  pad_len;
    uint8_t  next_hdr;
} __attribute__((packed));

/* Security Association (SA) database */
#define SADB_MAX_SAS 16

enum sadb_mode {
    SADB_MODE_TRANSPORT = 1,
    SADB_MODE_TUNNEL    = 2,
};

enum sadb_proto {
    SADB_PROTO_AH  = 1,
    SADB_PROTO_ESP = 2,
};

struct security_assoc {
    int      in_use;
    uint8_t  proto;           /* AH or ESP */
    uint8_t  mode;            /* transport or tunnel */
    uint32_t spi;
    uint32_t src_ip;
    uint32_t dst_ip;
    /* Key material (simplified — single key for auth/encrypt) */
    uint8_t  auth_key[32];
    int      auth_key_len;
    uint8_t  enc_key[32];
    int      enc_key_len;
    /* Anti-replay window */
    uint32_t replay_win;      /* bitmap of last 32 sequence numbers */
    uint32_t last_seq;        /* last received sequence number */
};

static struct security_assoc sadb[SADB_MAX_SAS];
static int ipsec_initialised = 0;

void ipsec_init(void)
{
    if (ipsec_initialised) return;
    memset(sadb, 0, sizeof(sadb));
    ipsec_initialised = 1;
    kprintf("[OK] ipsec: IPsec AH/ESP subsystem initialised (%d SAs max)\n",
            SADB_MAX_SAS);
}

/* Find a free SA slot */
static int sadb_find_free(void)
{
    for (int i = 0; i < SADB_MAX_SAS; i++) {
        if (!sadb[i].in_use) return i;
    }
    return -ENOSPC;
}

/* Find SA by SPI and destination IP */
static int sadb_find_by_spi(uint32_t spi, uint32_t dst_ip)
{
    for (int i = 0; i < SADB_MAX_SAS; i++) {
        if (sadb[i].in_use && sadb[i].spi == spi && sadb[i].dst_ip == dst_ip)
            return i;
    }
    return -ENOENT;
}

/* Add a Security Association */
int ipsec_sa_add(uint32_t spi, uint32_t src_ip, uint32_t dst_ip,
                 uint8_t proto, uint8_t mode,
                 const uint8_t *auth_key, int auth_key_len,
                 const uint8_t *enc_key, int enc_key_len)
{
    if (!ipsec_initialised) return -ENOSYS;
    if (proto != SADB_PROTO_AH && proto != SADB_PROTO_ESP) return -EINVAL;
    if (mode != SADB_MODE_TRANSPORT && mode != SADB_MODE_TUNNEL) return -EINVAL;

    int idx = sadb_find_free();
    if (idx < 0) return -ENOSPC;

    struct security_assoc *sa = &sadb[idx];
    memset(sa, 0, sizeof(*sa));
    sa->in_use = 1;
    sa->proto = proto;
    sa->mode = mode;
    sa->spi = spi;
    sa->src_ip = src_ip;
    sa->dst_ip = dst_ip;

    if (auth_key && auth_key_len > 0) {
        int len = auth_key_len > 32 ? 32 : auth_key_len;
        memcpy(sa->auth_key, auth_key, len);
        sa->auth_key_len = len;
    }
    if (enc_key && enc_key_len > 0) {
        int len = enc_key_len > 32 ? 32 : enc_key_len;
        memcpy(sa->enc_key, enc_key, len);
        sa->enc_key_len = len;
    }

    kprintf("ipsec: added SA spi=0x%x %d.%d.%d.%d -> %d.%d.%d.%d proto=%s mode=%s\n",
            spi,
            (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
            (src_ip >> 8) & 0xFF, src_ip & 0xFF,
            (dst_ip >> 24) & 0xFF, (dst_ip >> 16) & 0xFF,
            (dst_ip >> 8) & 0xFF, dst_ip & 0xFF,
            proto == SADB_PROTO_AH ? "AH" : "ESP",
            mode == SADB_MODE_TRANSPORT ? "transport" : "tunnel");
    return 0;
}

/* Delete a Security Association */
int ipsec_sa_del(uint32_t spi, uint32_t dst_ip)
{
    int idx = sadb_find_by_spi(spi, dst_ip);
    if (idx < 0) return -ENOENT;
    memset(&sadb[idx], 0, sizeof(sadb[idx]));
    return 0;
}

/* Flush all SAs */
void ipsec_sa_flush(void)
{
    memset(sadb, 0, sizeof(sadb));
    kprintf("ipsec: all SAs flushed\n");
}

/* Process inbound AH header (verify ICV placeholder) */
static int ipsec_input_ah(struct ip_header *ip_hdr, const uint8_t *payload)
{
    (void)ip_hdr;
    struct ah_header *ah = (struct ah_header *)payload;
    int idx = sadb_find_by_spi(ah->spi, ip_hdr->src_ip);
    if (idx < 0) {
        kprintf("ipsec: no SA for inbound AH spi=0x%x\n", ah->spi);
        return -ENOENT;
    }
    kprintf("ipsec: inbound AH packet spi=0x%x seq=%u\n", ah->spi, ah->seq_no);
    /* Anti-replay check (simplified) */
    if (ah->seq_no <= sadb[idx].last_seq) {
        kprintf("ipsec: replay attempt dropped (seq=%u last=%u)\n",
                ah->seq_no, sadb[idx].last_seq);
        return -EINVAL;
    }
    sadb[idx].last_seq = ah->seq_no;
    return 0;
}

/* Process inbound ESP header (decrypt placeholder) */
static int ipsec_input_esp(struct ip_header *ip_hdr, const uint8_t *payload)
{
    (void)ip_hdr;
    struct esp_header *esp = (struct esp_header *)payload;
    int idx = sadb_find_by_spi(esp->spi, ip_hdr->src_ip);
    if (idx < 0) {
        kprintf("ipsec: no SA for inbound ESP spi=0x%x\n", esp->spi);
        return -ENOENT;
    }
    kprintf("ipsec: inbound ESP packet spi=0x%x seq=%u\n", esp->spi, esp->seq_no);
    if (esp->seq_no <= sadb[idx].last_seq) {
        kprintf("ipsec: replay attempt dropped (seq=%u last=%u)\n",
                esp->seq_no, sadb[idx].last_seq);
        return -EINVAL;
    }
    sadb[idx].last_seq = esp->seq_no;
    return 0;
}

/* Process inbound IPsec packet (called from IP layer) */
void handle_ipsec_ah(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len)
{
    (void)len;
    if (!ipsec_initialised) return;
    ipsec_input_ah(ip_hdr, payload);
}

void handle_ipsec_esp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len)
{
    (void)len;
    if (!ipsec_initialised) return;
    ipsec_input_esp(ip_hdr, payload);
}
#include "module.h"
module_init(ipsec_init);
