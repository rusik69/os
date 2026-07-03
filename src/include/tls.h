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

/* ── TLS Handshake Message Types (RFC 8446 §4, RFC 5246 §7.4) ────── */
#define TLS_HT_HELLO_REQUEST          0
#define TLS_HT_CLIENT_HELLO           1
#define TLS_HT_SERVER_HELLO           2
#define TLS_HT_HELLO_VERIFY_REQUEST   3
#define TLS_HT_NEW_SESSION_TICKET     4
#define TLS_HT_END_OF_EARLY_DATA      5
#define TLS_HT_ENCRYPTED_EXTENSIONS   8
#define TLS_HT_CERTIFICATE           11
#define TLS_HT_SERVER_KEY_EXCHANGE   12
#define TLS_HT_CERTIFICATE_REQUEST   13
#define TLS_HT_SERVER_HELLO_DONE     14
#define TLS_HT_CERTIFICATE_VERIFY    15
#define TLS_HT_CLIENT_KEY_EXCHANGE   16
#define TLS_HT_FINISHED              20
#define TLS_HT_KEY_UPDATE            24
#define TLS_HT_MESSAGE_HASH         254

/* ── TLS Handshake Message Header (4 bytes on wire) ───────────────── */
struct tls_handshake_header {
	uint8_t  msg_type;       /* TLS_HT_* */
	uint8_t  length[3];      /* length of body (big-endian, 24-bit) */
} __attribute__((packed));

#define TLS_HANDSHAKE_HEADER_LEN  4
#define TLS_MAX_HANDSHAKE_LEN     (1 << 24)  /* 16 MB — 3-byte length field */

/* ── TLS Cipher Suites (IANA) ──────────────────────────────────────── */
/* TLS 1.3 cipher suites */
#define TLS_AES_128_GCM_SHA256        0x1301
#define TLS_AES_256_GCM_SHA384        0x1302
#define TLS_CHACHA20_POLY1305_SHA256  0x1303
#define TLS_AES_128_CCM_SHA256        0x1304
#define TLS_AES_128_CCM_8_SHA256      0x1305

/* TLS 1.2 cipher suites */
#define TLS_RSA_WITH_AES_128_CBC_SHA     0x002F
#define TLS_RSA_WITH_AES_256_CBC_SHA     0x0035
#define TLS_ECDHE_RSA_WITH_AES_128_GCM   0xC02F
#define TLS_ECDHE_ECDSA_WITH_AES_128_GCM 0xC02B
#define TLS_ECDHE_RSA_WITH_CHACHA20      0xCCA8
#define TLS_DHE_RSA_WITH_AES_128_GCM     0x009E

/* ── TLS Hello Extensions (RFC 8446 §4.2, RFC 5246 §7.4.1) ───────── */

/* Extension type codes */
#define TLS_EXT_SERVER_NAME            0x0000
#define TLS_EXT_MAX_FRAGMENT_LEN      0x0001
#define TLS_EXT_STATUS_REQUEST         0x0005
#define TLS_EXT_SUPPORTED_GROUPS       0x000A
#define TLS_EXT_EC_POINT_FORMATS       0x000B
#define TLS_EXT_SIGNATURE_ALGORITHMS   0x000D
#define TLS_EXT_ALPN                   0x0010
#define TLS_EXT_QUIC_TRANSPORT         0x0039
#define TLS_EXT_ENCRYPT_THEN_MAC       0x0016
#define TLS_EXT_EXTENDED_MASTER_SECRET 0x0017
#define TLS_EXT_SESSION_TICKET         0x0023
#define TLS_EXT_KEY_SHARE              0x002A
#define TLS_EXT_SUPPORTED_VERSIONS     0x002B
#define TLS_EXT_PSK_KEY_EXCHANGE_MODES 0x002D
#define TLS_EXT_PSK                    0x0029

/* ── TLS Handshake Message Macros ────────────────────────────────────── */

/* Write a 3-byte big-endian length into the handshake header */
static inline void tls_hs_set_length(struct tls_handshake_header *hdr, int len)
{
	hdr->length[0] = (uint8_t)((len >> 16) & 0xFF);
	hdr->length[1] = (uint8_t)((len >>  8) & 0xFF);
	hdr->length[2] = (uint8_t)((len >>  0) & 0xFF);
}

/* Read a 3-byte big-endian length from the handshake header */
static inline int tls_hs_get_length(const struct tls_handshake_header *hdr)
{
	return ((int)hdr->length[0] << 16) |
	       ((int)hdr->length[1] <<  8) |
	        (int)hdr->length[2];
}

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
	/* ── Handshake state ──────────────────────────────────────────── */
	uint8_t  random[32];              /* peer's ClientHello/ServerHello random */
	uint8_t  session_id[32];          /* session ID (echoed by server) */
	uint8_t  session_id_len;          /* length of session ID (0..32) */
};

/* ── Handshake Protocol API ──────────────────────────────────────────── */

/* Build a handshake message frame: header + body.
 * Returns the total size of the frame (header + body), or negative errno. */
int tls_hs_build_frame(uint8_t msg_type, const uint8_t *body, int body_len,
                       uint8_t *out, int out_cap);

/* Parse a handshake message header from raw input.
 * Returns the total frame size (header + body) on success,
 * or negative errno on failure.  Sets *msg_type and *body_len. */
int tls_hs_parse_header(const uint8_t *in, int in_len,
                        uint8_t *msg_type, int *body_len);

/* ── ClientHello API ─────────────────────────────────────────────────── */

/* Build a ClientHello message.
 * Supported cipher suites are listed in 'ciphers' (uint16_t array).
 * Returns bytes written to 'out', or negative errno. */
int tls_build_client_hello(struct tls_conn *conn,
                           const uint16_t *ciphers, int num_ciphers,
                           const uint8_t *session_id, int session_id_len,
                           uint8_t *out, int out_cap);

/* Parse a ClientHello message from raw bytes.
 * On success returns the body length, or negative errno.
 * Extracts: version, random, session_id (into conn),
 * and sets *cipher_choices / *num_choices. */
int tls_parse_client_hello(struct tls_conn *conn,
                           const uint8_t *body, int body_len,
                           uint16_t *cipher_choices, int *num_choices,
                           int max_choices);

/* ── ServerHello API ─────────────────────────────────────────────────── */

/* Build a ServerHello message.
 * The server selects a cipher suite from the client's list.
 * Returns bytes written to 'out', or negative errno. */
int tls_build_server_hello(struct tls_conn *conn,
                           uint16_t selected_cipher,
                           const uint8_t *session_id, int session_id_len,
                           uint8_t *out, int out_cap);

/* Parse a ServerHello message from raw bytes.
 * On success returns the body length, or negative errno.
 * Extracts: version, random, session_id, cipher_suite. */
int tls_parse_server_hello(struct tls_conn *conn,
                           const uint8_t *body, int body_len);

/* ── Handshake State Machine ─────────────────────────────────────────── */

enum tls_hs_state {
	TLS_HS_IDLE = 0,
	TLS_HS_CLIENT_HELLO_SENT,
	TLS_HS_SERVER_HELLO_SENT,
	TLS_HS_SERVER_PARAMS_SENT,
	TLS_HS_SERVER_DONE,
	TLS_HS_CLIENT_FINISHED,
	TLS_HS_SERVER_FINISHED,
	TLS_HS_CONNECTED,
	TLS_HS_ERROR = -1,
};

/* Advance the TLS handshake state machine.
 * For the client: returns a handshake message to send (as TLS_CT_HANDSHAKE
 * record payload).  For the server: processes an incoming handshake message
 * and returns the next message to send.
 *
 * 'in' / 'in_len' — received handshake data (NULL for the first call).
 * 'out' / 'out_cap' — buffer for the next handshake message to send.
 *
 * Returns the number of bytes written to 'out' (< 0 on error).
 * Sets *new_state to indicate the new handshake state. */
int tls_handshake_step(struct tls_conn *conn, enum tls_hs_state *state,
                       const uint8_t *in, int in_len,
                       uint8_t *out, int out_cap);

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
