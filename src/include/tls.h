/* tls.h — TLS record layer: content types, record structure, encryption API */

#ifndef TLS_H
#define TLS_H

#include "types.h"

/* ── TLS Content Types (RFC 8446 §4, RFC 5246 §6.2.1) ─────────────── */
#define TLS_CT_CHANGE_CIPHER_SPEC  20
#define TLS_CT_ALERT               21
#define TLS_CT_HANDSHAKE           22
#define TLS_CT_APPLICATION_DATA    23
#define TLS_CT_HEARTBEAT           24

/* ── TLS Protocol Versions (wire format: major.minor) ──────────────── */
#define TLS_VER_1_0   0x0301
#define TLS_VER_1_1   0x0302
#define TLS_VER_1_2   0x0303
#define TLS_VER_1_3   0x0304

/* ── Record Layer Limits (RFC 8446 §5.1) ──────────────────────────── */
#define TLS_MAX_PLAINTEXT_LEN   16384   /* 2^14 — max payload per record */
#define TLS_MAX_COMPRESSED_LEN  (TLS_MAX_PLAINTEXT_LEN + 1024)
#define TLS_MAX_CIPHERTEXT_LEN  (TLS_MAX_COMPRESSED_LEN + 1024)
#define TLS_MAX_RECORD_LEN      (TLS_MAX_CIPHERTEXT_LEN + 256)

/* ── TLS Record Header (5 bytes on wire) ──────────────────────────── */
struct tls_record_header {
	uint8_t  content_type;    /* TLS_CT_* */
	uint16_t version;         /* TLS_VER_* — network byte order */
	uint16_t length;          /* length of fragment (network byte order) */
} __attribute__((packed));

#define TLS_RECORD_HEADER_LEN  5

/* ── TLS Cipher State (per-connection, per-direction) ──────────────── */
struct tls_cipher_state {
	/* Encryption key material */
	uint8_t  enc_key[32];
	int      enc_key_len;
	/* IV / nonce material */
	uint8_t  fixed_iv[4];     /* implicit part (TLS 1.2 AEAD nonce) */
	uint8_t  explicit_iv[8];  /* explicit per-record nonce */
	/* MAC key material */
	uint8_t  mac_key[32];
	int      mac_key_len;
	/* Record sequence number (monotonic per direction) */
	uint64_t seq_num;
	/* Negotiated cipher suite identifier */
	uint16_t cipher_suite;
	/* 1 = encryption active, 0 = still in cleartext (early handshake) */
	int      is_active;
};

/* ── TLS Connection State ────────────────────────────────────────────── */
struct tls_conn {
	struct tls_cipher_state rstate;   /* read  cipher state */
	struct tls_cipher_state wstate;   /* write cipher state */
	uint16_t version;                 /* negotiated protocol version */
	int      is_client;               /* 1 = client role, 0 = server */
	/* Reassembly buffer for partially-received records */
	uint8_t  recv_buf[TLS_MAX_PLAINTEXT_LEN + 64];
	int      recv_len;
};

/* ── Record Layer API ────────────────────────────────────────────────── */

/* Initialise the TLS subsystem (call once at boot) */
int tls_init(void);

/* Initialise a TLS connection structure */
int tls_conn_init(struct tls_conn *conn, int is_client, uint16_t version);

/* Clean up a TLS connection (zeroises crypto material) */
void tls_conn_cleanup(struct tls_conn *conn);

/* Build one or more TLS records from plaintext, handling fragmentation
 * and encryption.  Returns bytes written to 'out', or negative errno.
 *
 * For data > TLS_MAX_PLAINTEXT_LEN, multiple records are emitted. */
int tls_record_send(struct tls_conn *conn, uint8_t content_type,
                    const uint8_t *data, int data_len,
                    uint8_t *out, int out_cap);

/* Parse and decrypt a single TLS record.  Returns the content type
 * in *content_type and the decrypted payload in 'data'.
 * Returns the number of payload bytes, or negative errno. */
int tls_record_recv(struct tls_conn *conn,
                    const uint8_t *in, int in_len,
                    uint8_t *content_type,
                    uint8_t *data, int data_cap);

/* Encrypt a single TLS record fragment.
 * Internal helper exposed for unit testing. */
int tls_record_encrypt(struct tls_cipher_state *cs,
                       uint8_t content_type, uint16_t version,
                       const uint8_t *plain, int plain_len,
                       uint8_t *out, int out_cap);

/* Decrypt a single TLS record given the header + ciphertext.
 * Internal helper exposed for unit testing. */
int tls_record_decrypt(struct tls_cipher_state *cs,
                       const struct tls_record_header *hdr,
                       const uint8_t *cipher, int cipher_len,
                       uint8_t *data, int data_cap);

#endif /* TLS_H */
