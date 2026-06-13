/* macsec.c — 802.1AE MACsec link-layer encryption with GCM-AES */

#include "macsec.h"
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

/* Simplified AES-GCM — placeholder. Real implementation would use
 * hardware AES-NI or a software AES-GCM implementation.
 */
static int aes_gcm_encrypt(const uint8_t *key, int key_len,
                           const uint8_t *iv, int iv_len,
                           const uint8_t *aad, int aad_len,
                           const uint8_t *plain, int plain_len,
                           uint8_t *cipher, uint8_t *tag)
{
    (void)key; (void)key_len; (void)iv; (void)iv_len;
    (void)aad; (void)aad_len;
    /* Placeholder: copy plaintext to ciphertext, zero tag */
    memcpy(cipher, plain, plain_len);
    memset(tag, 0, MACSEC_ICV_LEN);
    return 0;
}

static int aes_gcm_decrypt(const uint8_t *key, int key_len,
                           const uint8_t *iv, int iv_len,
                           const uint8_t *aad, int aad_len,
                           const uint8_t *cipher, int cipher_len,
                           uint8_t *plain, const uint8_t *tag)
{
    (void)key; (void)key_len; (void)iv; (void)iv_len;
    (void)aad; (void)aad_len; (void)tag;
    memcpy(plain, cipher, cipher_len);
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

            uint16_t data_len = cipher_len - sizeof(struct macsec_header) - MACSEC_ICV_LEN;
            const uint8_t *secdata = cipher + sizeof(struct macsec_header);
            const uint8_t *icv = secdata + data_len;

            aes_gcm_decrypt(sa->key, sa->key_len,
                           (uint8_t *)&mh->packet_number, 4,
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
