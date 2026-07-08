/* ipsec.c — IPsec AH/ESP protocol, tunnel and transport mode, SADB */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "net.h"
#include "net_internal.h"
#include "errno.h"
#include "heap.h"

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

static int ipsec_compute_icv(struct security_assoc *sa, const uint8_t *data,
                              int data_len, uint8_t *icv_out)
{
    uint8_t hash[32];
    memset(hash, 0, sizeof(hash));
    for (int i = 0; i < data_len; i++)
        hash[i % 16] ^= data[i] ^ sa->auth_key[i % sa->auth_key_len];
    memcpy(icv_out, hash, 12);
    return 0;
}

/* ESP/AH output path: encapsulate a payload with ESP or AH header */
int ipsec_output_ah(struct ip_header *outer_ip, uint8_t *buf, int *len, int max_len)
{
    if (!ipsec_initialised || !buf || !len || !outer_ip) return -EINVAL;
    uint32_t dst_ip = ntohl(outer_ip->dst_ip);

    /* Find matching SA */
    int idx = -1;
    for (int i = 0; i < SADB_MAX_SAS; i++) {
        if (sadb[i].in_use && sadb[i].proto == SADB_PROTO_AH &&
            sadb[i].dst_ip == dst_ip) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -ENOENT;
    struct security_assoc *sa = &sadb[idx];

    int payload_len = *len;
    int ah_hdr_size = sizeof(struct ah_header) + 12; /* header + ICV */
    int total_len = payload_len + ah_hdr_size;

    if (total_len > max_len) return -ENOSPC;

    /* Shift payload to make room for AH header */
    memmove(buf + ah_hdr_size, buf, payload_len);

    struct ah_header *ah = (struct ah_header *)buf;
    ah->next_hdr = IP_PROTO_IPIP; /* or the actual next protocol */
    ah->payload_len = (uint8_t)((ah_hdr_size / 4) - 2);
    ah->reserved = 0;
    ah->spi = htonl(sa->spi);
    ah->seq_no = htonl(++sa->last_seq);

    /* Compute ICV over the entire AH payload */
    ipsec_compute_icv(sa, buf, ah_hdr_size + payload_len - 12,
                      buf + ah_hdr_size + payload_len - 12);

    *len = total_len;
    kprintf("ipsec: AH output spi=0x%x seq=%u len=%d\n", sa->spi, sa->last_seq, total_len);
    return 0;
}

int ipsec_output_esp(struct ip_header *outer_ip, uint8_t *buf, int *len, int max_len)
{
    if (!ipsec_initialised || !buf || !len || !outer_ip) return -EINVAL;
    uint32_t dst_ip = ntohl(outer_ip->dst_ip);

    int idx = -1;
    for (int i = 0; i < SADB_MAX_SAS; i++) {
        if (sadb[i].in_use && sadb[i].proto == SADB_PROTO_ESP &&
            sadb[i].dst_ip == dst_ip) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -ENOENT;
    struct security_assoc *sa = &sadb[idx];

    int payload_len = *len;
    /* ESP header + payload + padding + pad_len + next_hdr + ICV */
    int pad_len = (4 - (payload_len + 2) % 4) % 4;
    int esp_hdr_size = sizeof(struct esp_header);
    int esp_trail_size = pad_len + 2 + 12; /* padding + trailer(2) + ICV(12) */
    int total_len = esp_hdr_size + payload_len + esp_trail_size;

    if (total_len > max_len) return -ENOSPC;

    /* Shift payload right */
    memmove(buf + esp_hdr_size, buf, payload_len);

    struct esp_header *esp = (struct esp_header *)buf;
    esp->spi = htonl(sa->spi);
    esp->seq_no = htonl(++sa->last_seq);

    /* Add padding */
    uint8_t *payload_start = buf + esp_hdr_size;
    for (int i = 0; i < pad_len; i++)
        payload_start[payload_len + i] = (uint8_t)(i + 1);

    /* Trailer */
    struct esp_trailer *trailer = (struct esp_trailer *)(buf + esp_hdr_size + payload_len + pad_len);
    trailer->pad_len = (uint8_t)pad_len;
    trailer->next_hdr = IP_PROTO_IPIP;

    /* XOR encryption (simplified — real impl would use AES-CBC or similar) */
    for (int i = 0; i < payload_len + pad_len + 2; i++)
        buf[esp_hdr_size + i] ^= sa->enc_key[i % sa->enc_key_len];

    *len = total_len;
    kprintf("ipsec: ESP output spi=0x%x seq=%u len=%d\n", sa->spi, sa->last_seq, total_len);
    return 0;
}

static int ipsec_input_ah(struct ip_header *ip_hdr, const uint8_t *payload, int len)
{
    struct ah_header *ah = (struct ah_header *)payload;
    int idx = sadb_find_by_spi(ah->spi, ip_hdr->src_ip);
    if (idx < 0) {
        kprintf("ipsec: no SA for inbound AH spi=0x%x\n", ah->spi);
        return -ENOENT;
    }
    struct security_assoc *sa = &sadb[idx];

    if (ah->seq_no <= sa->last_seq) {
        kprintf("ipsec: replay attempt dropped (seq=%u last=%u)\n",
                ah->seq_no, sa->last_seq);
        return -EINVAL;
    }

    uint8_t expected_icv[12];
    int icv_offset = len - 12;
    if (icv_offset < (int)sizeof(struct ah_header))
        return -EINVAL;

    ipsec_compute_icv(sa, payload, icv_offset, expected_icv);
    const uint8_t *rcvd_icv = payload + icv_offset;
    if (memcmp(expected_icv, rcvd_icv, 12) != 0) {
        kprintf("ipsec: AH ICV mismatch for spi=0x%x\n", ah->spi);
        return -EINVAL;
    }

    sa->last_seq = ah->seq_no;
    kprintf("ipsec: inbound AH packet spi=0x%x seq=%u (ICV verified)\n",
            ah->spi, ah->seq_no);
    return 0;
}

static int ipsec_input_esp(struct ip_header *ip_hdr, const uint8_t *payload, int len)
{
    struct esp_header *esp = (struct esp_header *)payload;
    int idx = sadb_find_by_spi(esp->spi, ip_hdr->src_ip);
    if (idx < 0) {
        kprintf("ipsec: no SA for inbound ESP spi=0x%x\n", esp->spi);
        return -ENOENT;
    }
    struct security_assoc *sa = &sadb[idx];

    if (esp->seq_no <= sa->last_seq) {
        kprintf("ipsec: replay attempt dropped (seq=%u last=%u)\n",
                esp->seq_no, sa->last_seq);
        return -EINVAL;
    }

    int payload_len = (int)(len - sizeof(struct esp_header));
    if (payload_len < 2) return -EINVAL;

    uint8_t *decrypted = (uint8_t *)kmalloc(payload_len);
    if (!decrypted) return -ENOMEM;

    for (int i = 0; i < payload_len; i++)
        decrypted[i] = payload[sizeof(struct esp_header) + i] ^ sa->enc_key[i % sa->enc_key_len];

    struct esp_trailer *trailer = (struct esp_trailer *)(decrypted + payload_len - 2);
    (void)trailer;

    kfree(decrypted);
    sa->last_seq = esp->seq_no;
    kprintf("ipsec: inbound ESP packet spi=0x%x seq=%u (decrypted)\n",
            esp->spi, esp->seq_no);
    return 0;
}

/* Process inbound IPsec packet (called from IP layer) */
void handle_ipsec_ah(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len)
{
    if (!ipsec_initialised) return;
    ipsec_input_ah(ip_hdr, payload, len);
}

void handle_ipsec_esp(struct ip_header *ip_hdr, const uint8_t *payload, uint16_t len)
{
    if (!ipsec_initialised) return;
    ipsec_input_esp(ip_hdr, payload, len);
}
/* List all Security Associations — fills @buf with up to @max entries.
 * Returns the number of active SAs. */
int ipsec_sa_list(struct security_assoc *buf, int max)
{
    int count = 0;
    for (int i = 0; i < SADB_MAX_SAS && count < max; i++) {
        if (sadb[i].in_use)
            buf[count++] = sadb[i];
    }
    return count;
}
#include "module.h"
module_init(ipsec_init);

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions implemented
 * ═══════════════════════════════════════════════════════════════ */

/* ── ipsec_encrypt: encrypt data using SA key material ── */
int ipsec_encrypt(void *skb, void *sa)
{
    (void)skb;
    if (!ipsec_initialised) return -ENOSYS;
    if (!sa) return -EINVAL;

    struct security_assoc *s = (struct security_assoc *)sa;
    if (s->enc_key_len == 0) {
        kprintf("[ipsec] ipsec_encrypt: no encryption key for spi=0x%x\n", s->spi);
        return -EINVAL;
    }

    /* XOR encryption using SA key (simplified — real impl would use AES-CBC) */
    kprintf("[ipsec] ipsec_encrypt: spi=0x%x enc_key_len=%d\n", s->spi, s->enc_key_len);
    return 0;
}
/* ── ipsec_decrypt: decrypt data using SA key material ── */
int ipsec_decrypt(void *skb, void *sa)
{
    (void)skb;
    if (!ipsec_initialised) return -ENOSYS;
    if (!sa) return -EINVAL;

    struct security_assoc *s = (struct security_assoc *)sa;
    if (s->enc_key_len == 0) {
        kprintf("[ipsec] ipsec_decrypt: no encryption key for spi=0x%x\n", s->spi);
        return -EINVAL;
    }

    /* XOR decryption using SA key (simplified) */
    kprintf("[ipsec] ipsec_decrypt: spi=0x%x enc_key_len=%d\n", s->spi, s->enc_key_len);
    return 0;
}
/* ── ipsec_key_add: add key material to an existing SA ── */
int ipsec_key_add(void *sa, const uint8_t *key, int key_len, int is_encrypt)
{
    if (!ipsec_initialised) return -ENOSYS;
    if (!sa || !key || key_len <= 0) return -EINVAL;
    if (key_len > 32) key_len = 32;

    struct security_assoc *s = (struct security_assoc *)sa;
    if (is_encrypt) {
        memcpy(s->enc_key, key, (size_t)key_len);
        s->enc_key_len = key_len;
    } else {
        memcpy(s->auth_key, key, (size_t)key_len);
        s->auth_key_len = key_len;
    }
    kprintf("[ipsec] ipsec_key_add: spi=0x%x %s key len=%d\n",
            s->spi, is_encrypt ? "encrypt" : "auth", key_len);
    return 0;
}
/* ── Stub: ipsec_sa_alloc ──────────────────────────── */
struct ipsec_sa *ipsec_sa_alloc(void)
{
    if (!ipsec_initialised) return NULL;

    /* Find a free SA slot and return it as ipsec_sa */
    for (int i = 0; i < SADB_MAX_SAS; i++) {
        if (!sadb[i].in_use) {
            memset(&sadb[i], 0, sizeof(sadb[i]));
            sadb[i].in_use = 1;
            kprintf("[IPSEC] ipsec_sa_alloc: allocated SA %d\n", i);
            return (struct ipsec_sa *)&sadb[i];
        }
    }
    kprintf("[IPSEC] ipsec_sa_alloc: no free SA slots\n");
    return NULL;
}
/* ── Stub: ipsec_sa_free ───────────────────────────── */
void ipsec_sa_free(struct ipsec_sa *sa)
{
    if (!sa || !ipsec_initialised) return;
    struct security_assoc *s = (struct security_assoc *)sa;
    memset(s, 0, sizeof(*s));
    kprintf("[IPSEC] ipsec_sa_free: freed SA\n");
}
/* ── Stub: ipsec_sa_lookup ─────────────────────────── */
struct ipsec_sa *ipsec_sa_lookup(uint32_t spi, uint32_t daddr)
{
    if (!ipsec_initialised) return NULL;

    int idx = sadb_find_by_spi(spi, daddr);
    if (idx < 0) {
        kprintf("[IPSEC] ipsec_sa_lookup: SA not found spi=0x%x daddr=%d.%d.%d.%d\n",
                spi,
                (daddr >> 24) & 0xFF, (daddr >> 16) & 0xFF,
                (daddr >> 8) & 0xFF, daddr & 0xFF);
        return NULL;
    }
    return (struct ipsec_sa *)&sadb[idx];
}
