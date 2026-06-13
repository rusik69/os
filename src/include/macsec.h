#ifndef MACSEC_H
#define MACSEC_H

#include "types.h"

/* MACsec (802.1AE) — Link-layer encryption with GCM-AES
 * No AF number — operates at Ethernet layer using a special EtherType
 */

#define ETH_P_MACSEC        0x88E5  /* IEEE 802.1AE MACsec EtherType */

/* MACsec header */
struct macsec_header {
    uint8_t  tci_an;          /* TCI (3) | AN (2) | reserved (3) — upper byte */
    uint8_t  short_len;       /* if ES=0&SC=0: SL; if ES|SC: reserved */
    uint8_t  reserved[2];
    uint32_t packet_number;   /* Packet Number (PN) */
    /* Secure Channel Identifier (SCI) — present if SC=1 in TCI */
    /* Secure Data follows (original EtherType + payload) */
    /* Integrity Check Value (ICV) — 16 bytes at end */
} __attribute__((packed));

/* TCI (Tag Control Information) bit positions */
#define MACSEC_TCI_ES       0x40  /* End station (no SCI) */
#define MACSEC_TCI_SC       0x20  /* SCI present */
#define MACSEC_TCI_SCB      0x10  /* Single Copy Broadcast */
#define MACSEC_TCI_E        0x08  /* Encrypted */
#define MACSEC_TCI_C        0x04  /* Changed text */
#define MACSEC_AN_MASK      0x03  /* Association Number mask */

/* MACsec cipher suite */
#define MACSEC_CIPHER_GCM_AES_128   0x0080C20001000001ULL  /* Default GCM-AES-128 */
#define MACSEC_CIPHER_GCM_AES_256   0x0080C20001000002ULL  /* GCM-AES-256 */
#define MACSEC_CIPHER_GCM_AES_XPN_128 0x0080C20001000003ULL /* Extended PN */
#define MACSEC_CIPHER_GCM_AES_XPN_256 0x0080C20001000004ULL

/* Key length */
#define MACSEC_KEY_LEN_128  16
#define MACSEC_KEY_LEN_256  32
#define MACSEC_ICV_LEN      16  /* Integrity Check Value length */

/* Secure Channel */
#define MACSEC_MAX_SC       4
#define MACSEC_MAX_SA       4   /* Security Associations per SC */

struct macsec_sa {
    int     used;
    uint8_t key[32];            /* AES key (up to 256 bits) */
    uint8_t key_len;            /* 16 or 32 */
    uint32_t next_pn;           /* Next packet number */
    uint32_t lowest_pn;         /* Lowest acceptable PN (anti-replay) */
    uint8_t  active;            /* 1 = currently in use for transmit */
};

struct macsec_sc {
    int     used;
    uint64_t sci;               /* Secure Channel Identifier */
    uint8_t  mac[6];            /* Associated MAC address */
    struct macsec_sa sa[MACSEC_MAX_SA];
    uint8_t  active_sa_an;      /* Active association number (0-3) */
    uint8_t  encrypt;           /* 1 = encrypt, 0 = integrity-only */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
};

/* API */
void macsec_init(void);
int  macsec_create_sc(uint64_t sci, const uint8_t *mac, int encrypt);
int  macsec_delete_sc(uint64_t sci);
int  macsec_create_sa(uint64_t sci, uint8_t an, const uint8_t *key, uint8_t key_len);
int  macsec_activate_sa(uint64_t sci, uint8_t an);
int  macsec_encrypt(uint64_t sci, const uint8_t *plain, uint16_t plain_len,
                    uint8_t *out, uint16_t *out_len);
int  macsec_decrypt(uint64_t sci, const uint8_t *cipher, uint16_t cipher_len,
                    uint8_t *out, uint16_t *out_len);
int  macsec_handle_frame(const uint8_t *frame, uint16_t len,
                         uint8_t *out, uint16_t *out_len);

#endif /* MACSEC_H */
