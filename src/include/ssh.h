#ifndef SSH_H
#define SSH_H

#include "types.h"

/* ── SSH version ────────────────────────────────────────────── */
#define SSH_VERSION_STR   "SSH-2.0-OSSSH_1.0"
#define SSH_VERSION_LEN   22

/* ── SSH message types ──────────────────────────────────────── */
#define SSH_MSG_DISCONNECT                1
#define SSH_MSG_IGNORE                    2
#define SSH_MSG_UNIMPLEMENTED             3
#define SSH_MSG_DEBUG                     4
#define SSH_MSG_SERVICE_REQUEST           5
#define SSH_MSG_SERVICE_ACCEPT            6
#define SSH_MSG_KEXINIT                  20
#define SSH_MSG_NEWKEYS                  21
#define SSH_MSG_KEXDH_INIT               30
#define SSH_MSG_KEXDH_REPLY              31
#define SSH_MSG_KEX_DH_GEX_GROUP         31
#define SSH_MSG_KEX_DH_GEX_INIT          32
#define SSH_MSG_KEX_DH_GEX_REPLY         33
#define SSH_MSG_KEX_DH_GEX_REQUEST       34
#define SSH_MSG_USERAUTH_REQUEST         50
#define SSH_MSG_USERAUTH_FAILURE         51
#define SSH_MSG_USERAUTH_SUCCESS         52
#define SSH_MSG_USERAUTH_BANNER          53
#define SSH_MSG_GLOBAL_REQUEST           80
#define SSH_MSG_REQUEST_SUCCESS          81
#define SSH_MSG_REQUEST_FAILURE          82
#define SSH_MSG_CHANNEL_OPEN             90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE     92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST    93
#define SSH_MSG_CHANNEL_DATA             94
#define SSH_MSG_CHANNEL_EXTENDED_DATA    95
#define SSH_MSG_CHANNEL_EOF              96
#define SSH_MSG_CHANNEL_CLOSE            97
#define SSH_MSG_CHANNEL_REQUEST          98
#define SSH_MSG_CHANNEL_SUCCESS          99
#define SSH_MSG_CHANNEL_FAILURE         100

/* ── Disconnect reasons ─────────────────────────────────────── */
#define SSH_DISCONNECT_HOST_NOT_ALLOWED_TO_CONNECT  1
#define SSH_DISCONNECT_PROTOCOL_ERROR               2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED          3
#define SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE      4
#define SSH_DISCONNECT_ILLEGAL_USER                 7
#define SSH_DISCONNECT_AUTH_CANCELLED_BY_USER       13
#define SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE 14

/* ── Channel open failure reasons ──────────────────────────── */
#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED  1
#define SSH_OPEN_CONNECT_FAILED               2
#define SSH_OPEN_UNKNOWN_CHANNEL_TYPE         3
#define SSH_OPEN_RESOURCE_SHORTAGE            4

/* ── Algorithm name strings ────────────────────────────────── */
#define SSH_KEX_ALGO    "diffie-hellman-group14-sha256"
#define SSH_HOSTKEY_ALGO "ssh-rsa"
#define SSH_CIPHER_ALGO "aes128-cbc"
#define SSH_MAC_ALGO    "hmac-sha2-256"
#define SSH_COMP_ALGO   "none"

/* ── Crypto constants ──────────────────────────────────────── */
#define SSH_MAX_PACKET     32768
#define SSH_BLOCK_SIZE     16    /* AES block */
#define SSH_MAC_SIZE       32    /* SHA-256 MAC */
#define SSH_KEY_SIZE       16    /* AES-128 */
#define SSH_IV_SIZE        16
#define SSH_SESSION_ID_LEN 32

/* ── KEX DH group 14 prime (2048-bit, RFC 3526) ────────────── */
#define DH14_P_HEX \
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1" \
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD" \
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C24" \
    "5E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7" \
    "EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45" \
    "B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24" \
    "CF5F83655D23DCA3AD961C62F356208552BB9ED52907709" \
    "6966D670C354E4ABC9804F1746C08CA237327FFFFFFFFFFFF"
#define DH14_G 2

/* ── ECC DH group 14 value size ────────────────────────────── */
#define DH14_MPINT_SIZE 2049  /* max 257 bytes (2048 bits + leading 0x00) */

/* ── Big number helpers (2048-bit max) ─────────────────────── */
#define BN_MAX_LIMBS 64  /* 64 x 32-bit = 2048 bits */
#define BN_MAX_BYTES 256

typedef struct {
    uint32_t l[BN_MAX_LIMBS];
    int used;
} bignum;

/* ── SSH session state ─────────────────────────────────────── */
struct ssh_session;

/* ── Public API ─────────────────────────────────────────────── */
/* Server */
int  sshd_start(void);
void sshd_stop(void);
void sshd_init(void);
void sshd_task(void);

#endif /* SSH_H */
