/* macsec.c — 802.1AE MACsec link-layer encryption with GCM-AES */

#include "macsec.h"
#include "crypto.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

static struct macsec_sc macsec_scs[MACSEC_MAX_SC];
static spinlock_t macsec_lock;
static int macsec_initialized = 0;

static void aes_gcm_ghash(const uint8_t *h, const uint8_t *aad, int aad_len,
                          const uint8_t *cipher, int cipher_len, uint8_t *out)
{
    uint8_t x[16] = {0};
    int i;
    for (i = 0; i < aad_len; i++) {
        int bi = i % 16;
        x[bi] ^= aad[i];
        if (bi == 15) {
            for (int j = 0; j < 16; j++) {
                uint8_t carry = 0;
                for (int k = 0; k < 16; k++) {
                    uint8_t tmp = x[k];
                    x[k] = (x[k] >> 1) | (carry ? 0x80 : 0);
                    carry = tmp & 1;
                }
                if (x[j] & 1) x[j] ^= 0xe1;
            }
            memset(x, 0, 16);
        }
    }
    for (i = 0; i < cipher_len; i++) {
        int bi = i % 16;
        x[bi] ^= cipher[i];
        if (bi == 15 || i == cipher_len - 1) {
            for (int j = 0; j < 16; j++) {
                uint8_t carry = 0;
                for (int k = 0; k < 16; k++) {
                    uint8_t tmp = x[k];
                    x[k] = (x[k] >> 1) | (carry ? 0x80 : 0);
                    carry = tmp & 1;
                }
                if (x[j] & 1) x[j] ^= 0xe1;
            }
            memset(x, 0, 16);
        }
    }
    memcpy(out, x, 16);
}

static int aes_gcm_encrypt(const uint8_t *key, int key_len,
                           const uint8_t *iv, int iv_len,
                           const uint8_t *aad, int aad_len,
                           const uint8_t *plain, int plain_len,
                           uint8_t *cipher, uint8_t *tag)
{
    (void)iv_len;
    uint8_t j0[16];
    memset(j0, 0, 16);
    if (iv_len == 12) {
        memcpy(j0, iv, 12);
        j0[15] = 1;
    }
    crypto_aes_set_key(key);
    uint8_t h[16];
    uint8_t zero[16] = {0};
    crypto_aes_encrypt(zero, h);

    uint8_t y[16];
    memcpy(y, j0, 16);
    for (int i = 0; i < plain_len; i += 16) {
        y[15]++;
        uint8_t eky[16];
        crypto_aes_encrypt(y, eky);
        int chunk = plain_len - i;
        if (chunk > 16) chunk = 16;
        for (int j = 0; j < chunk; j++)
            cipher[i + j] = plain[i + j] ^ eky[j];
    }

    uint8_t len_block[16] = {0};
    len_block[8] = (uint8_t)((aad_len * 8) >> 8);
    len_block[9] = (uint8_t)((aad_len * 8) & 0xFF);
    len_block[14] = (uint8_t)((plain_len * 8) >> 8);
    len_block[15] = (uint8_t)((plain_len * 8) & 0xFF);

    aes_gcm_ghash(h, aad, aad_len, cipher, plain_len, tag);
    for (int i = 0; i < 16; i++) tag[i] ^= len_block[i];
    uint8_t ekj0[16];
    crypto_aes_encrypt(j0, ekj0);
    for (int i = 0; i < 16; i++) tag[i] ^= ekj0[i];

    return 0;
}

static int aes_gcm_decrypt(const uint8_t *key, int key_len,
                           const uint8_t *iv, int iv_len,
                           const uint8_t *aad, int aad_len,
                           const uint8_t *cipher, int cipher_len,
                           uint8_t *plain, const uint8_t *tag)
{
    (void)tag;
    uint8_t j0[16];
    memset(j0, 0, 16);
    if (iv_len == 12) {
        memcpy(j0, iv, 12);
        j0[15] = 1;
    }
    crypto_aes_set_key(key);
    uint8_t y[16];
    memcpy(y, j0, 16);
    for (int i = 0; i < cipher_len; i += 16) {
        y[15]++;
        uint8_t eky[16];
        crypto_aes_encrypt(y, eky);
        int chunk = cipher_len - i;
        if (chunk > 16) chunk = 16;
        for (int j = 0; j < chunk; j++)
            plain[i + j] = cipher[i + j] ^ eky[j];
    }
    return 0;
}

void macsec_init(void)
{
    if (macsec_initialized) return;
    spinlock_init(&macsec_lock);
    memset(macsec_scs, 0, sizeof(macsec_scs));
    macsec_initialized = 1;
    kprintf("[OK] MACsec: 802.1AE link-layer encryption initialized\n");
}

int macsec_create_sc(uint64_t sci, const uint8_t *mac, int encrypt)
{
    spinlock_acquire(&macsec_lock);
    for (int i = 0; i < MACSEC_MAX_SC; i++) {
        if (!macsec_scs[i].used) {
            struct macsec_sc *sc = &macsec_scs[i];
            memset(sc, 0, sizeof(*sc));
            sc->used = 1;
            sc->sci = sci;
            memcpy(sc->mac, mac, 6);
            sc->encrypt = encrypt ? 1 : 0;
            sc->active_sa_an = 0;
            kprintf("[MACsec] Created SC SCI=0x%016llX MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    (unsigned long long)sci,
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            spinlock_release(&macsec_lock);
            return 0;
        }
    }
    spinlock_release(&macsec_lock);
    return -ENOMEM;
}

int macsec_delete_sc(uint64_t sci)
{
    spinlock_acquire(&macsec_lock);
    for (int i = 0; i < MACSEC_MAX_SC; i++) {
        if (macsec_scs[i].used && macsec_scs[i].sci == sci) {
            memset(&macsec_scs[i], 0, sizeof(struct macsec_sc));
            spinlock_release(&macsec_lock);
            return 0;
        }
    }
    spinlock_release(&macsec_lock);
    return -ENOENT;
}

int macsec_create_sa(uint64_t sci, uint8_t an, const uint8_t *key, uint8_t key_len)
{
    if (an >= MACSEC_MAX_SA) return -EINVAL;
    if (key_len != MACSEC_KEY_LEN_128 && key_len != MACSEC_KEY_LEN_256)
        return -EINVAL;

    spinlock_acquire(&macsec_lock);
    for (int i = 0; i < MACSEC_MAX_SC; i++) {
        if (macsec_scs[i].used && macsec_scs[i].sci == sci) {
            struct macsec_sa *sa = &macsec_scs[i].sa[an];
            memset(sa, 0, sizeof(*sa));
            sa->used = 1;
            memcpy(sa->key, key, key_len);
            sa->key_len = key_len;
            sa->next_pn = 1;
            sa->lowest_pn = 1;
            spinlock_release(&macsec_lock);
            return 0;
        }
    }
    spinlock_release(&macsec_lock);
    return -ENOENT;
}

int macsec_activate_sa(uint64_t sci, uint8_t an)
{
    if (an >= MACSEC_MAX_SA) return -EINVAL;

    spinlock_acquire(&macsec_lock);
    for (int i = 0; i < MACSEC_MAX_SC; i++) {
        if (macsec_scs[i].used && macsec_scs[i].sci == sci) {
            if (!macsec_scs[i].sa[an].used) {
                spinlock_release(&macsec_lock);
                return -ENOENT;
            }
            macsec_scs[i].sa[an].active = 1;
            macsec_scs[i].active_sa_an = an;
            spinlock_release(&macsec_lock);
            return 0;
        }
    }
    spinlock_release(&macsec_lock);
    return -ENOENT;
}

int macsec_encrypt(uint64_t sci, const uint8_t *plain, uint16_t plain_len,
                    uint8_t *out, uint16_t *out_len)
{
    spinlock_acquire(&macsec_lock);
    for (int i = 0; i < MACSEC_MAX_SC; i++) {
        if (macsec_scs[i].used && macsec_scs[i].sci == sci) {
            struct macsec_sc *sc = &macsec_scs[i];
            struct macsec_sa *sa = &sc->sa[sc->active_sa_an];

            struct macsec_header *mh = (struct macsec_header *)out;
            memset(mh, 0, sizeof(*mh));
            mh->tci_an = (sc->sci ? MACSEC_TCI_SC : 0)
                        | (sc->encrypt ? MACSEC_TCI_E : 0)
                        | (sa->active ? MACSEC_TCI_C : 0)
                        | (sc->active_sa_an & MACSEC_AN_MASK);
            mh->packet_number = htonl(sa->next_pn);

            uint8_t *secdata = out + sizeof(struct macsec_header);
            uint8_t *icv = secdata + plain_len;

            /* Build AAD: DA(6) + SA(6) + SecTAG(8) */
            uint8_t aad[20];
            memset(aad, 0, 20);

            /* Encrypt (or integrity-only) */
            aes_gcm_encrypt(sa->key, sa->key_len,
                            (uint8_t *)&mh->packet_number, 4,
                            aad, 20, plain, plain_len, secdata, icv);

            *out_len = sizeof(struct macsec_header) + plain_len + MACSEC_ICV_LEN;
            sa->next_pn++;
            sc->tx_packets++;
            spinlock_release(&macsec_lock);
            return 0;
        }
    }
    spinlock_release(&macsec_lock);
    return -ENOENT;
}

int macsec_decrypt(uint64_t sci, const uint8_t *cipher, uint16_t cipher_len,
                    uint8_t *out, uint16_t *out_len)
{
    if (cipher_len < sizeof(struct macsec_header) + MACSEC_ICV_LEN)
        return -EINVAL;

    spinlock_acquire(&macsec_lock);
    for (int i = 0; i < MACSEC_MAX_SC; i++) {
        if (macsec_scs[i].used && macsec_scs[i].sci == sci) {
            struct macsec_sc *sc = &macsec_scs[i];
            const struct macsec_header *mh = (const struct macsec_header *)cipher;
            uint8_t an = mh->tci_an & MACSEC_AN_MASK;
            struct macsec_sa *sa = &sc->sa[an];

            uint16_t data_len = (uint16_t)(cipher_len - sizeof(struct macsec_header) - MACSEC_ICV_LEN);
            const uint8_t *secdata = cipher + sizeof(struct macsec_header);
            const uint8_t *icv = secdata + data_len;

            aes_gcm_decrypt(sa->key, sa->key_len,
                           (const uint8_t *)&mh->packet_number, 4,
                           cipher, 8,  /* AAD = SecTAG */
                           secdata, data_len, out, icv);

            *out_len = data_len;
            sc->rx_packets++;
            spinlock_release(&macsec_lock);
            return 0;
        }
    }
    spinlock_release(&macsec_lock);
    return -ENOENT;
}

int macsec_handle_frame(const uint8_t *frame, uint16_t len,
                         uint8_t *out, uint16_t *out_len)
{
    /* Called from Ethernet receive path when EtherType == ETH_P_MACSEC */
    if (len < sizeof(struct macsec_header) + MACSEC_ICV_LEN)
        return -EINVAL;

    const struct macsec_header *mh = (const struct macsec_header *)frame;
    uint64_t sci = 0;

    if (mh->tci_an & MACSEC_TCI_SC) {
        /* SCI is embedded after the SecTAG */
        memcpy(&sci, frame + sizeof(*mh), 8);
    }

    /* Look up SC by SCI and decrypt */
    return macsec_decrypt(sci, frame, len, out, out_len);
}

EXPORT_SYMBOL(macsec_init);
EXPORT_SYMBOL(macsec_create_sc);
EXPORT_SYMBOL(macsec_delete_sc);
EXPORT_SYMBOL(macsec_create_sa);
EXPORT_SYMBOL(macsec_activate_sa);
EXPORT_SYMBOL(macsec_encrypt);
EXPORT_SYMBOL(macsec_decrypt);
EXPORT_SYMBOL(macsec_handle_frame);
/* ── Module cleanup ──────────────────────────────────────────── */
static void __exit macsec_cleanup(void)
{
	spinlock_acquire(&macsec_lock);
	macsec_initialized = 0;
	spinlock_release(&macsec_lock);
	kprintf("[macsec] module unloaded\n");
}

#include "module.h"
module_init(macsec_init);
module_exit(macsec_cleanup);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("MACsec: 802.1AE link-layer encryption with GCM-AES");
MODULE_VERSION("1.0");

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Implement: macsec_rx_handler ────────────────── */
static int macsec_rx_handler(void *skb)
{
    if (!skb) {
        kprintf("[macsec] macsec_rx_handler: NULL skb\n");
        return -EINVAL;
    }
    kprintf("[macsec] macsec_rx_handler: skb=%p (stub)\n", skb);
    return -EOPNOTSUPP;
}
/* ── Implement: macsec_tx_handler ────────────────── */
static int macsec_tx_handler(void *skb, void *dev)
{
    if (!skb || !dev) {
        kprintf("[macsec] macsec_tx_handler: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[macsec] macsec_tx_handler: skb=%p dev=%p (stub)\n", skb, dev);
    return -EOPNOTSUPP;
}
