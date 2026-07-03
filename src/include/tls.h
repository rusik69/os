/* tls.h — TLS record layer: content types, record structure, encryption API */

#ifndef TLS_H
#define TLS_H

#include "types.h"
#include "sha256.h"

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

/* ── TLS ALPN (RFC 7301) — Application-Layer Protocol Negotiation ── */

/* Maximum length of a single ALPN protocol name (RFC 7301 §3.1:
 * protocol_name is 1..255 bytes) */
#define TLS_ALPN_MAX_NAME_LEN      255
/* Maximum number of ALPN protocols a client may offer */
#define TLS_ALPN_MAX_PROTOCOLS     16

/* ── ALPN wire format helpers (RFC 7301 §3.1) ───────────────────────
 *
 * ProtocolNameList wire format:
 *   uint16_t list_length;  // big-endian, includes all ProtocolName entries
 *   struct {
 *     uint8_t name_length; // 1..255
 *     uint8_t name[name_length];
 *   } protocol_name_list[];
 */

/* ── SNI (RFC 6066) — Server Name Indication ───────────────────────── */

/* Maximum length of a single SNI hostname (must fit in uint16_t length field) */
#define TLS_SNI_MAX_HOSTNAME_LEN      256

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
#define TLS_EXT_EARLY_DATA            0x002B
#define TLS_EXT_COOKIE                0x002C

/* ── Renegotiation Info Extension (RFC 5746) ───────────────────────── */
#define TLS_EXT_RENEGOTIATION_INFO    0xFF01

/* Verify data length for TLS 1.2 renegotiation (12 bytes per
 * RFC 5746 §3.5 — 12 bytes of client_verify_data or server_verify_data) */
#define TLS_RENEGO_VERIFY_DATA_LEN    12

/* ── TLS 1.3 0-RTT early data limits */
#define TLS_MAX_EARLY_DATA_SIZE       16384
#define TLS_DEFAULT_TICKET_LIFETIME   7200

/* ── Session Ticket Constants (RFC 5077, RFC 8446 §4.6.1) ──────────── */
#define TLS_TICKET_KEY_LEN         16     /* AES-128 key for ticket encryption */
#define TLS_TICKET_NONCE_LEN       16     /* per-ticket nonce */
#define TLS_TICKET_AAD_LEN         8      /* Additional authenticated data: timestamp(8) */
#define TLS_MAX_TICKET_LEN         1024   /* maximum encrypted ticket size */
#define TLS_MAX_SESSION_DATA_LEN   256    /* max plaintext session data */
#define TLS_TICKET_LIFETIME_DEFAULT 7200  /* default ticket lifetime (seconds) */
#define TLS_TICKET_LIFETIME_MAX    86400  /* max ticket lifetime (24h) */
#define TLS_NUM_TICKET_KEYS        4      /* rolling window of ticket keys */

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

/* ── TLS 1.3 Pre-Shared Key Structure (RFC 8446 §4.2.11) ────────────────── */
struct tls_psk {
	uint8_t  identity[256];
	int      identity_len;
	uint8_t  secret[32];
	uint8_t  nonce[16];
	int      nonce_len;
	uint64_t ticket_age_add;
	uint32_t lifetime;
	uint32_t max_early_data;
	uint32_t age;
	int      valid;
};

/* ── TLS 1.3 Early Data States ─────────────────────────────────────────── */
enum tls_early_data_state {
	TLS_ED_NONE = 0,
	TLS_ED_SENT,
	TLS_ED_RECEIVING,
	TLS_ED_DONE,
};

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
	/* TLS 1.3 early data (0-RTT) */
	struct tls_cipher_state early_wstate;
	struct tls_cipher_state early_rstate;
	struct tls_psk psk;
	uint32_t max_early_data;
	uint8_t  transcript_hash[32];
	int      transcript_hash_len;
	int      has_psk;
	int      early_data_active;
	enum tls_early_data_state early_state;
	/* ── TLS renegotiation (RFC 5746) ─────────────────────────────── */
	int      renego_in_progress;        /* 1 = renegotiation handshake active */
	int      renego_allowed;            /* 0 = reject renegotiation requests */
	uint8_t  renego_client_verify_data[TLS_RENEGO_VERIFY_DATA_LEN];
	uint8_t  renego_server_verify_data[TLS_RENEGO_VERIFY_DATA_LEN];
	/* ── ALPN (RFC 7301) — Application-Layer Protocol Negotiation ─── */
	uint8_t  alpn_protos[TLS_ALPN_MAX_PROTOCOLS][TLS_ALPN_MAX_NAME_LEN];
	uint8_t  alpn_proto_lens[TLS_ALPN_MAX_PROTOCOLS];
	int      alpn_num_protos;
	uint8_t  alpn_selected[TLS_ALPN_MAX_NAME_LEN];
	uint8_t  alpn_selected_len;
	int      alpn_negotiated;
	/* ── SNI (RFC 6066) — Server Name Indication ──────────────── */
	uint8_t  sni_hostname[TLS_SNI_MAX_HOSTNAME_LEN];
	uint8_t  sni_hostname_len;
	int      sni_negotiated;
};

/* ── TLS 1.3 Early Data Replay Protection Context ──────────────────────── */
struct tls_early_data_ctx {
	uint8_t  client_early_secret[32];
	uint8_t  server_early_secret[32];
	uint8_t  early_exporter_secret[32];
	int      is_derived;
	uint64_t earliest_allowed_time;
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
	TLS_HS_EARLY_DATA_SENT,
	TLS_HS_WAITING_EARLY_DATA,
	TLS_HS_SERVER_HELLO_SENT,
	TLS_HS_SERVER_PARAMS_SENT,
	TLS_HS_SERVER_DONE,
	TLS_HS_CLIENT_FINISHED,
	TLS_HS_SERVER_FINISHED,
	TLS_HS_TICKET_SENT,
	TLS_HS_CONNECTED,
	TLS_HS_RENEGO_WAIT_HELLO,   /* server: sent HelloRequest, awaiting client's new ClientHello */
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

/* ── Cipher Suite Negotiation (TLS 1.2) ───────────────────────────────── */

/* Key exchange algorithm identifiers */
enum tls_kx_algorithm {
	TLS_KX_NONE    = 0,
	TLS_KX_RSA     = 1,   /* RSA key transport (RFC 5246 §7.4.7.1) */
	TLS_KX_DHE     = 2,   /* Ephemeral Diffie-Hellman (RFC 5246 §7.4.7.2) */
	TLS_KX_ECDHE   = 3,   /* Ephemeral ECDH   (RFC 4492 §5.4) */
};

/* Authentication algorithm identifiers */
enum tls_auth_algorithm {
	TLS_AUTH_NONE  = 0,
	TLS_AUTH_RSA   = 1,
	TLS_AUTH_DSS   = 2,
	TLS_AUTH_ECDSA = 3,
	TLS_AUTH_PSK   = 4,
};

/* Encryption algorithm identifiers */
enum tls_enc_algorithm {
	TLS_ENC_NONE           = 0,
	TLS_ENC_NULL           = 1,
	TLS_ENC_AES_128_CBC    = 2,
	TLS_ENC_AES_256_CBC    = 3,
	TLS_ENC_AES_128_GCM    = 4,
	TLS_ENC_AES_256_GCM    = 5,
	TLS_ENC_CHACHA20_POLY1305 = 6,
};

/* MAC / PRF algorithm identifiers */
enum tls_mac_algorithm {
	TLS_MAC_NULL   = 0,
	TLS_MAC_SHA    = 1,   /* HMAC-SHA1   (RFC 5246) */
	TLS_MAC_SHA256 = 2,   /* HMAC-SHA256 (RFC 5246) */
	TLS_MAC_SHA384 = 3,   /* HMAC-SHA384 (RFC 5246) */
	TLS_MAC_AEAD   = 4,   /* AEAD — no separate MAC (RFC 5288) */
};

/* Cipher suite descriptor */
struct tls_cipher_suite_info {
	uint16_t        cipher_suite;      /* IANA code point */
	enum tls_kx_algorithm   kx_algo;       /* key exchange */
	enum tls_auth_algorithm auth_algo;      /* authentication */
	enum tls_enc_algorithm  enc_algo;       /* encryption */
	enum tls_mac_algorithm  mac_algo;       /* MAC / PRF */
	int             enc_key_len;     /* symmetric key size (bytes) */
	int             fixed_iv_len;    /* implicit IV / nonce size */
	int             tag_len;         /* AEAD auth tag (0 for non-AEAD) */
	int             is_aead;         /* 1 = AEAD cipher, 0 = CBC+HMAC */
};

/* ── Negotiation API ──────────────────────────────────────────────────── */

/* Look up cipher suite info by IANA code point.
 * Returns a pointer to the info struct, or NULL if unknown. */
const struct tls_cipher_suite_info *
tls_cipher_suite_lookup(uint16_t cipher_suite);

/* Negotiate a cipher suite between client-offered and server-preferred sets.
 *
 * 'client_suites' — cipher suites offered by the client (from ClientHello).
 * 'num_client'    — number of entries in client_suites.
 * 'server_prefs'  — server's preference list (NULL = use default).
 * 'num_server'    — number of entries in server_prefs (0 = use default).
 *
 * Returns the cipher suite code point on success, or negative errno
 * if no mutually-supported suite is found (-ENOENT) or on invalid args. */
int tls_negotiate_cipher_suite(const uint16_t *client_suites, int num_client,
                               const uint16_t *server_prefs, int num_server);

/* ── Server Preference Table (default priority-ordered list) ────────── */

/* Default TLS 1.2 server cipher suite preference order.
 * Highest security / forward secrecy first. */
extern const uint16_t tls_default_server_prefs[];
extern const int      tls_default_server_prefs_count;

/* ── HKDF (RFC 5869) for TLS 1.3 Key Schedule ──────────────────────────── */
void tls_hkdf_extract(const uint8_t *salt, int salt_len,
                      const uint8_t *ikm, int ikm_len,
                      uint8_t *prk);
void tls_hkdf_expand(const uint8_t *prk, int prk_len,
                     const uint8_t *info, int info_len,
                     uint8_t *out, int out_len);
void tls_hkdf_expand_label(const uint8_t *secret, int secret_len,
                           const char *label,
                           const uint8_t *context, int context_len,
                           uint8_t *out, int out_len);
void tls_derive_secret(const uint8_t *secret, const char *label,
                       const uint8_t *transcript_hash,
                       uint8_t *out);

/* ── TLS 1.3 Early Data (0-RTT) API ───────────────────────────────────── */
int tls_derive_early_traffic_keys(struct tls_conn *conn,
                                  const struct tls_psk *psk,
                                  const uint8_t *client_hello,
                                  int client_hello_len);
int tls_build_end_of_early_data(uint8_t *out, int out_cap);
int tls_parse_end_of_early_data(const uint8_t *in, int in_len);
int tls_build_early_data_ext(uint32_t max_early_data_size,
                              uint8_t *out, int out_cap);
int tls_early_data_encrypt(struct tls_conn *conn,
                           const uint8_t *data, int data_len,
                           uint8_t *out, int out_cap);
int tls_early_data_decrypt(struct tls_conn *conn,
                           const uint8_t *in, int in_len,
                           uint8_t *data, int data_cap);
void tls_early_data_init(struct tls_conn *conn);

/* ─────────────────────────────────────────────────────────────────────────
 * TLS Session Resumption — Session Tickets (RFC 5077, RFC 8446 §4.6.1)
 * ─────────────────────────────────────────────────────────────────────── */

/* Session ticket encryption key */
struct tls_ticket_key {
	uint8_t  key[TLS_TICKET_KEY_LEN];  /* AES-128 key */
	uint8_t  hmac_key[32];             /* HMAC-SHA256 key for integrity */
	uint32_t key_id;                   /* key identifier (sent with ticket) */
	uint64_t generation_time;          /* monotonic time of key generation */
	int      active;                   /* 1 = currently active */
};

/* Session ticket key context (global, server-side) */
struct tls_ticket_ctx {
	struct tls_ticket_key keys[TLS_NUM_TICKET_KEYS];
	int      num_keys;
	int      current_key;             /* index into keys[] of active key */
	uint64_t current_time;            /* approximate monotonic time */
};

/* Session data stored inside an encrypted ticket (not directly on the wire).
 * When the ticket is decrypted/validated, the server reconstructs this
 * from the AES-GCM plaintext. */
struct tls_session_ticket_data {
	uint16_t protocol_version;         /* TLS version of original session */
	uint16_t cipher_suite;             /* negotiated cipher suite */
	uint8_t  psk_secret[32];           /* PSK for TLS 1.3 resumption */
	uint8_t  session_id[32];           /* session ID (TLS 1.2) */
	uint8_t  session_id_len;
	uint64_t timestamp;                /* ticket creation time */
	uint8_t  resumption_master_secret[32]; /* TLS 1.2 resumption MS */
};

/* ── Session Ticket API ────────────────────────────────────────────── */

/* Initialise the session ticket subsystem (call once at boot) */
int tls_session_init(void);

/* Get the global ticket key context */
const struct tls_ticket_ctx *tls_get_ticket_ctx(void);

/* Generate/rotate ticket keys */
int tls_ticket_key_rotate(struct tls_ticket_ctx *ctx);
int tls_ticket_key_init(struct tls_ticket_ctx *ctx);

/* Create an encrypted session ticket from session data.
 * Returns the ticket length (including AEAD tag), or negative errno. */
int tls_ticket_create(const struct tls_ticket_ctx *ctx,
                      const struct tls_session_ticket_data *data,
                      uint8_t *ticket_out, int ticket_cap,
                      uint32_t *ticket_age_add, uint32_t *key_id);

/* Parse and validate an encrypted session ticket.
 * On success returns 0 and populates *data. */
int tls_ticket_parse(const struct tls_ticket_ctx *ctx,
                     const uint8_t *ticket, int ticket_len,
                     uint32_t key_id,
                     struct tls_session_ticket_data *data);

/* Build a TLS 1.3 NewSessionTicket handshake message body.
 * Returns bytes written to 'out', or negative errno. */
int tls_build_new_session_ticket(const uint8_t *ticket_data, int ticket_len,
                                 uint32_t ticket_lifetime,
                                 uint32_t ticket_age_add,
                                 const uint8_t *ticket_nonce, int nonce_len,
                                 uint8_t *out, int out_cap);

/* Parse a NewSessionTicket message body.
 * Returns 0 on success, negative errno on failure. */
int tls_parse_new_session_ticket(const uint8_t *body, int body_len,
                                 uint32_t *ticket_lifetime,
                                 uint32_t *ticket_age_add,
                                 const uint8_t **ticket, int *ticket_len,
                                 const uint8_t **nonce, int *nonce_len);

/* Build TLS 1.3 PSK extension for ClientHello (RFC 8446 §4.2.11).
 * ticket  — the session ticket to present as a PSK identity
 * ticket_len — length of the ticket
 * obfuscated_age — client's view of ticket age (obfuscated_ticket_age)
 * out/out_cap — output buffer
 * Returns bytes written, or negative errno. */
int tls_build_psk_extension(const uint8_t *ticket, int ticket_len,
                            uint32_t obfuscated_age,
                            uint8_t *out, int out_cap);

/* Parse TLS 1.3 PSK extension from ClientHello.
 * Extracts the first PSK identity (ticket) from the extension.
 * On success returns 0 and sets *ticket/_len to point into body. */
int tls_parse_psk_extension(const uint8_t *ext_body, int ext_len,
                            const uint8_t **ticket, int *ticket_len);

/* Build TLS 1.3 PSK key exchange modes extension (RFC 8446 §4.2.9).
 * modes — bitmask of supported psk_key_exchange_mode values
 *          (bit 0 = psk_ke, bit 1 = psk_dhe_ke)
 * out/out_cap — output buffer
 * Returns bytes written, or negative errno. */
int tls_build_psk_ke_modes_ext(int modes,
                                uint8_t *out, int out_cap);

/* ── X.509 Certificate Parsing ────────────────────────────────────── */

/* Initialise the X.509 certificate parser (call once at boot) */
int tls_x509_init(void);

/* ── TLS Certificate Message (RFC 8446 §4.4.2) ─────────────────────── */

/* Maximum certificate chain depth we support */
#define TLS_MAX_CERT_CHAIN_DEPTH  8

/* A single entry in the Certificate message's certificate_list */
struct tls_cert_entry {
	const uint8_t *data;        /* raw DER certificate data */
	int            data_len;
	const uint8_t *extensions;  /* extensions blob (may be NULL) */
	int            ext_len;
};

/* Build a TLS Certificate message body (RFC 8446 §4.4.2).
 * 'certs' is an array of certificate entries to include.
 * Returns bytes written to 'out', or negative errno. */
int tls_build_certificate_msg(const struct tls_cert_entry *certs,
                               int num_certs,
                               uint8_t *out, int out_cap);

/* Parse a TLS Certificate message body.
 * Fills 'certs' array with pointers into 'body' (caller must keep body alive).
 * Returns 0 on success, or negative errno on parse failure. */
int tls_parse_certificate_msg(const uint8_t *body, int body_len,
                               struct tls_cert_entry *certs,
                               int max_certs, int *num_certs);

/* ── TLS Renegotiation (RFC 5246 §7.4.1, RFC 5746) ──────────────────── */

/* Build a HelloRequest handshake message (RFC 5246 §7.4.1.1).
 * HelloRequest is a handshake message with msg_type = 0 and an empty body.
 * Returns bytes written to 'out', or negative errno. */
int tls_build_hello_request(uint8_t *out, int out_cap);

/* Parse a HelloRequest handshake message.
 * Returns 0 on success, or negative errno if the message is not a valid
 * HelloRequest. */
int tls_parse_hello_request(const uint8_t *in, int in_len);

/* Build the renegotiation_info extension (RFC 5746 §3.2).
 * The extension contains the client's verify_data from the previous
 * Finished message(s) concatenated with the server's verify_data.
 * Returns bytes written to 'out', or negative errno. */
int tls_build_renegotiation_info_ext(const uint8_t *client_verify_data,
                                     int client_verify_len,
                                     const uint8_t *server_verify_data,
                                     int server_verify_len,
                                     uint8_t *out, int out_cap);

/* Parse the renegotiation_info extension (RFC 5746 §3.3).
 * On success returns the number of bytes of verify_data found,
 * and sets *data / *data_len to point into the extension body.
 * Returns negative errno on failure. */
int tls_parse_renegotiation_info_ext(const uint8_t *ext_body, int ext_body_len,
                                     const uint8_t **verify_data,
                                     int *verify_data_len);

/* Request TLS renegotiation from the application layer.
 * For a server: builds a HelloRequest in 'out'.
 * For a client: builds a new ClientHello with the renegotiation_info
 * extension in 'out'.
 * Returns bytes written to 'out' on success, negative errno on failure.
 * The caller is responsible for sending the built data and continuing
 * to call tls_handshake_step() for the renegotiation handshake. */
int tls_conn_request_renegotiate(struct tls_conn *conn,
                                  uint8_t *out, int out_cap);

/* Check whether a renegotiation is currently in progress.
 * Returns 1 if yes, 0 otherwise. */
static inline int tls_conn_is_renegotiating(const struct tls_conn *conn)
{
	return conn ? conn->renego_in_progress : 0;
}

/* Enable or disable renegotiation support on a connection.
 * By default renegotiation is enabled for TLS 1.2 and disabled for
 * TLS 1.3 (TLS 1.3 does not support renegotiation per RFC 8446 §4.1.1). */
static inline void tls_conn_set_renego_allowed(struct tls_conn *conn,
                                                int allowed)
{
	if (conn)
		conn->renego_allowed = allowed;
}

/* ── ALPN (RFC 7301) API ────────────────────────────────────────────── */

/* Build the ALPN extension (TLS_EXT_ALPN = 0x0010) for a ClientHello
 * or ServerHello.  The extension body contains:
 *   uint16_t list_length (big-endian)
 *   for each protocol: uint8_t name_length + uint8_t name[name_length]
 *
 * @protos:       array of protocol name strings
 * @proto_lens:   array of protocol name lengths (1..255 each)
 * @num_protos:   number of protocols in the list
 * @out:          output buffer for the extension (type + data length + body)
 * @out_cap:      capacity of output buffer
 *
 * Returns bytes written to 'out', or negative errno. */
int tls_build_alpn_ext(const uint8_t (*protos)[TLS_ALPN_MAX_NAME_LEN],
                       const uint8_t *proto_lens, int num_protos,
                       uint8_t *out, int out_cap);

/* Parse the ALPN extension from the extension body (after the
 * 2-byte ext_type + 2-byte ext_data_len).
 *
 * On success, stores the first protocol name matching `server_protos`
 * (if non-NULL and num_server > 0) into conn->alpn_selected, or if
 * server_protos is NULL stores the first protocol from the list.
 *
 * @ext_body:     the extension data (after the 2-byte ext_type and
 *                2-byte ext_data_len)
 * @ext_body_len: length of extension data
 * @conn:         TLS connection state (alpn_selected/len filled on match)
 * @server_protos:  server's supported protocol list (NULL = accept any)
 * @num_server:     number of server protocols
 *
 * Returns 0 on success (protocol negotiated or no ALPN extension found),
 * or negative errno on parse error.  Sets conn->alpn_negotiated = 1 when
 * a protocol is successfully selected. */
int tls_parse_alpn_ext(const uint8_t *ext_body, int ext_body_len,
                       struct tls_conn *conn,
                       const uint8_t (*server_protos)[TLS_ALPN_MAX_NAME_LEN],
                       const uint8_t *server_lens, int num_server);

/* Set the offered ALPN protocols on a TLS connection.
 * This is called by the application before starting the handshake.
 *
 * @conn:         TLS connection
 * @protos:       array of protocol name strings
 * @proto_lens:   array of protocol name lengths
 * @num_protos:   number of protocols
 *
 * Returns 0 on success, or negative errno. */
int tls_alpn_set_protocols(struct tls_conn *conn,
                           const uint8_t (*protos)[TLS_ALPN_MAX_NAME_LEN],
                           const uint8_t *proto_lens, int num_protos);

/* Get the negotiated ALPN protocol from a TLS connection.
 * Returns a pointer to the selected protocol name and sets *len,
 * or returns NULL if ALPN has not been negotiated. */
const uint8_t *tls_alpn_get_selected(const struct tls_conn *conn,
                                     uint8_t *len);

/* ── SNI (RFC 6066) — Server Name Indication API ────────────────────── */

/* Build the SNI extension for a ClientHello.
 *
 * The SNI extension (type 0x0000) contains a ServerNameList with one
 * host_name entry (NameType 0).  This function writes the full extension
 * including the 4-byte extension header (type + data length).
 *
 * @hostname:     the server name (ASCII, e.g. "www.example.com")
 * @hostname_len: length of the hostname string
 * @out:          output buffer for the extension
 * @out_cap:      capacity of output buffer
 *
 * Returns bytes written to 'out', or negative errno. */
int tls_build_sni_ext(const char *hostname, int hostname_len,
                      uint8_t *out, int out_cap);

/* Parse the SNI extension from the extension body (after the
 * 2-byte ext_type + 2-byte ext_data_len).
 *
 * On success, stores the first host_name entry into conn->sni_hostname.
 *
 * @ext_body:     the extension data (after ext_type and ext_data_len)
 * @ext_body_len: length of extension data
 * @conn:         TLS connection state (sni_hostname/len filled on match)
 *
 * Returns 0 on success, or negative errno on parse error.
 * Sets conn->sni_negotiated = 1 when a hostname is extracted. */
int tls_parse_sni_ext(const uint8_t *ext_body, int ext_body_len,
                      struct tls_conn *conn);

/* Set the SNI hostname on a TLS connection (client-side).
 * This is called by the application before starting the handshake
 * to indicate the desired server hostname.
 *
 * @conn:     TLS connection
 * @hostname: the server name string (ASCII)
 * @len:      length of the hostname string
 *
 * Returns 0 on success, or negative errno. */
int tls_sni_set_hostname(struct tls_conn *conn,
                         const char *hostname, int len);

/* Get the SNI hostname from a TLS connection (server-side).
 * Returns a pointer to the hostname string, or NULL if SNI was
 * not received or is empty. */
const char *tls_sni_get_hostname(const struct tls_conn *conn);

#endif /* TLS_H */
