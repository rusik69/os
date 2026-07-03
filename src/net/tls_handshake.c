/* tls_handshake.c — TLS handshake protocol: ClientHello, ServerHello
 *
 * Implements the TLS 1.2/1.3 handshake message framing, ClientHello
 * construction/parsing, and ServerHello construction/parsing, as well
 * as a lightweight state machine to drive the initial handshake flow.
 *
 * References:
 *   RFC 5246 §7.4 — TLS 1.2 Handshake Protocol
 *   RFC 8446 §4   — TLS 1.3 Handshake Protocol
 */

#include "tls.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "export.h"

/* ── Handshake Message Framing ─────────────────────────────────────── */

/*
 * tls_hs_build_frame — Build a handshake message frame.
 *
 * Writes the 4-byte handshake header followed by body into 'out'.
 * Returns the total number of bytes written (header + body), or a
 * negative errno on failure.
 */
int tls_hs_build_frame(uint8_t msg_type, const uint8_t *body, int body_len,
                       uint8_t *out, int out_cap)
{
	struct tls_handshake_header *hdr;
	int total;

	if (!out)
		return -EINVAL;
	if (body_len < 0 || body_len > TLS_MAX_HANDSHAKE_LEN)
		return -EINVAL;
	if (body && body_len == 0)
		return -EINVAL;
	if (!body && body_len > 0)
		return -EINVAL;

	total = TLS_HANDSHAKE_HEADER_LEN + body_len;
	if (out_cap < total)
		return -ENOSPC;

	hdr = (struct tls_handshake_header *)out;
	hdr->msg_type = msg_type;
	tls_hs_set_length(hdr, body_len);

	if (body_len > 0)
		memcpy(out + TLS_HANDSHAKE_HEADER_LEN, body, (size_t)body_len);

	return total;
}

/*
 * tls_hs_parse_header — Parse a handshake message header.
 *
 * Given raw input that should start with a handshake header, validates
 * the framing and returns the total frame size (header + body).
 * Sets *msg_type and *body_len from the header.
 * Returns negative errno on failure.
 */
int tls_hs_parse_header(const uint8_t *in, int in_len,
                        uint8_t *msg_type, int *body_len)
{
	const struct tls_handshake_header *hdr;
	int len;

	if (!in || !msg_type || !body_len)
		return -EINVAL;
	if (in_len < TLS_HANDSHAKE_HEADER_LEN)
		return -EINVAL;

	hdr = (const struct tls_handshake_header *)in;
	*msg_type = hdr->msg_type;
	len = tls_hs_get_length(hdr);

	if (len < 0 || len > TLS_MAX_HANDSHAKE_LEN)
		return -EINVAL;

	*body_len = len;

	/* Ensure the claimed length fits within the input buffer */
	if (TLS_HANDSHAKE_HEADER_LEN + len > in_len)
		return -EINVAL;

	return TLS_HANDSHAKE_HEADER_LEN + len;
}

/* ── Handshake Random Generation ────────────────────────────────────── */

/*
 * tls_generate_random — Fill a 32-byte random with dummy data.
 *
 * The first 4 bytes encode the current Unix timestamp (a rough
 * monotonic clock substitute using a boot-time counter), and the
 * remaining 28 bytes are filled with a simple pseudo-random pattern
 * derived from a static seed.  In a production implementation this
 * would use a proper CSPRNG (rng_get_bytes()).
 */
static void tls_generate_random(uint8_t random[32])
{
	static uint64_t counter = 0;
	uint32_t seed;

	seed = (uint32_t)(counter++);

	/* Generate a simple pseudo-random fill */
	for (int i = 0; i < 32; i++) {
		seed = seed * 1103515245U + 12345U;
		random[i] = (uint8_t)(seed >> 16);
	}
}

/* ── ClientHello Construction ───────────────────────────────────────── */

/*
 * tls_build_client_hello — Build a ClientHello message body.
 *
 * The body is written into 'out' (starting after the handshake header).
 * On success returns the number of bytes written to 'out'.
 * The caller is responsible for wrapping the result in a handshake
 * frame (via tls_hs_build_frame) and a TLS record (via tls_record_send).
 */
int tls_build_client_hello(struct tls_conn *conn,
                           const uint16_t *ciphers, int num_ciphers,
                           const uint8_t *session_id, int session_id_len,
                           uint8_t *out, int out_cap)
{
	int offset = 0;
	uint16_t version_wire;
	int i;

	if (!conn || !ciphers || !out)
		return -EINVAL;
	if (num_ciphers <= 0 || num_ciphers > 128)
		return -EINVAL;
	if (session_id_len < 0 || session_id_len > 32)
		return -EINVAL;
	if (session_id_len > 0 && !session_id)
		return -EINVAL;

	/* Reserve space for the maximum body size we might need */
	/* 2 (version) + 32 (random) + 1 (sid len) + 32 (sid) +
	 * 2 (ciphers len) + 2*num_ciphers + 1 (comp len) + 1 (comp null) */
	if (out_cap < 73 + 2 * num_ciphers)
		return -ENOSPC;

	/* Protocol version */
	version_wire = htons(conn->version);
	memcpy(out + offset, &version_wire, 2);
	offset += 2;

	/* Random */
	tls_generate_random(out + offset);
	offset += 32;

	/* Session ID */
	out[offset++] = (uint8_t)session_id_len;
	if (session_id_len > 0) {
		memcpy(out + offset, session_id, (size_t)session_id_len);
		offset += session_id_len;
	}

	/* Cipher suites length + list */
	{
		uint16_t ciphers_len = htons((uint16_t)(num_ciphers * 2));
		memcpy(out + offset, &ciphers_len, 2);
		offset += 2;

		for (i = 0; i < num_ciphers; i++) {
			uint16_t cs = htons(ciphers[i]);
			memcpy(out + offset, &cs, 2);
			offset += 2;
		}
	}

	/* Compression methods (1 byte null compression) */
	out[offset++] = 1;      /* length */
	out[offset++] = 0;      /* null compression */

	return offset;
}

/* ── ClientHello Parsing ────────────────────────────────────────────── */

/*
 * tls_parse_client_hello — Parse a ClientHello message body.
 *
 * Extracts the offered version, random, session ID, and cipher suites
 * from the ClientHello body.  Stores the random and session ID into
 * conn, and writes the list of cipher suite values into cipher_choices[].
 *
 * On success returns the body length (positive).  On failure returns
 * a negative errno.
 */
int tls_parse_client_hello(struct tls_conn *conn,
                           const uint8_t *body, int body_len,
                           uint16_t *cipher_choices, int *num_choices,
                           int max_choices)
{
	int offset = 0;
	uint16_t ver;
	int sid_len;
	int ciphers_byte_len;
	int num_ciphers;
	int comp_len;
	int i;

	if (!conn || !body || !cipher_choices || !num_choices)
		return -EINVAL;
	if (body_len < 38)  /* minimum: 2 + 32 + 1 + 1 + 2 */
		return -EINVAL;
	if (max_choices <= 0)
		return -EINVAL;

	/* Protocol version */
	memcpy(&ver, body + offset, 2);
	conn->version = ntohs(ver);
	offset += 2;

	/* Random */
	memcpy(conn->random, body + offset, 32);
	offset += 32;

	/* Session ID */
	sid_len = body[offset++];
	if (sid_len > 32)
		return -EINVAL;
	if (offset + sid_len > body_len)
		return -EINVAL;
	conn->session_id_len = (uint8_t)sid_len;
	if (sid_len > 0) {
		memcpy(conn->session_id, body + offset, (size_t)sid_len);
		offset += sid_len;
	} else {
		conn->session_id_len = 0;
	}

	/* Cipher suites */
	if (offset + 2 > body_len)
		return -EINVAL;
	memcpy(&ciphers_byte_len, body + offset, 2);
	ciphers_byte_len = ntohs((uint16_t)ciphers_byte_len);
	offset += 2;

	if (ciphers_byte_len < 2 || (ciphers_byte_len % 2) != 0)
		return -EINVAL;
	if (offset + ciphers_byte_len > body_len)
		return -EINVAL;

	num_ciphers = ciphers_byte_len / 2;
	if (num_ciphers > max_choices)
		num_ciphers = max_choices;

	for (i = 0; i < num_ciphers; i++) {
		uint16_t cs;
		memcpy(&cs, body + offset, 2);
		cipher_choices[i] = ntohs(cs);
		offset += 2;
	}
	*num_choices = num_ciphers;

	/* Skip any remaining cipher suite bytes we didn't read */
	if (ciphers_byte_len > num_ciphers * 2)
		offset += (ciphers_byte_len - num_ciphers * 2);

	/* Compression methods */
	if (offset + 1 > body_len)
		return -EINVAL;
	comp_len = body[offset++];
	if (offset + comp_len > body_len)
		return -EINVAL;
	/* Skip compression methods — we only support null (0) */
	offset += comp_len;

	/* Extensions — skip for now; parsed in task 10+ */
	/* (optional, may not be present) */

	(void)max_choices;

	return body_len;
}

/* ── ServerHello Construction ───────────────────────────────────────── */

/*
 * tls_build_server_hello — Build a ServerHello message body.
 *
 * The server selects a cipher suite and sends back its own random,
 * optionally echoing the client's session ID.  The body is written
 * to 'out'.  Returns the number of bytes written, or negative errno.
 */
int tls_build_server_hello(struct tls_conn *conn,
                           uint16_t selected_cipher,
                           const uint8_t *session_id, int session_id_len,
                           uint8_t *out, int out_cap)
{
	int offset = 0;
	uint16_t version_wire;
	uint16_t cipher_wire;

	if (!conn || !out)
		return -EINVAL;
	if (session_id_len < 0 || session_id_len > 32)
		return -EINVAL;
	if (session_id_len > 0 && !session_id)
		return -EINVAL;

	/* Minimum body: 2 (ver) + 32 (rand) + 1 (sid_len) + 0..32 (sid) +
	 * 2 (cipher) + 1 (comp) */
	if (out_cap < 38 + session_id_len)
		return -ENOSPC;

	/* Protocol version */
	version_wire = htons(conn->version);
	memcpy(out + offset, &version_wire, 2);
	offset += 2;

	/* Random */
	tls_generate_random(out + offset);
	offset += 32;

	/* Session ID (echoed from client or new) */
	out[offset++] = (uint8_t)session_id_len;
	if (session_id_len > 0) {
		memcpy(out + offset, session_id, (size_t)session_id_len);
		offset += session_id_len;
	}

	/* Selected cipher suite */
	cipher_wire = htons(selected_cipher);
	memcpy(out + offset, &cipher_wire, 2);
	offset += 2;

	/* Compression method (null only) */
	out[offset++] = 0;

	/* Store the negotiated cipher suite in the connection state */
	conn->wstate.cipher_suite = selected_cipher;

	return offset;
}

/* ── ServerHello Parsing ────────────────────────────────────────────── */

/*
 * tls_parse_server_hello — Parse a ServerHello message body.
 *
 * Extracts the server's chosen version, random, session ID, and cipher
 * suite from the ServerHello body.  Stores results into conn.
 *
 * On success returns the body length (positive).  On failure returns
 * a negative errno.
 */
int tls_parse_server_hello(struct tls_conn *conn,
                           const uint8_t *body, int body_len)
{
	int offset = 0;
	uint16_t ver;
	int sid_len;
	uint16_t cipher;

	if (!conn || !body)
		return -EINVAL;
	if (body_len < 38)  /* minimum: 2 + 32 + 1 + 2 + 1 */
		return -EINVAL;

	/* Protocol version */
	memcpy(&ver, body + offset, 2);
	conn->version = ntohs(ver);
	offset += 2;

	/* Random */
	memcpy(conn->random, body + offset, 32);
	offset += 32;

	/* Session ID */
	sid_len = body[offset++];
	if (sid_len > 32)
		return -EINVAL;
	if (offset + sid_len > body_len)
		return -EINVAL;
	conn->session_id_len = (uint8_t)sid_len;
	if (sid_len > 0) {
		memcpy(conn->session_id, body + offset, (size_t)sid_len);
		offset += sid_len;
	} else {
		conn->session_id_len = 0;
	}

	/* Cipher suite */
	if (offset + 2 > body_len)
		return -EINVAL;
	memcpy(&cipher, body + offset, 2);
	conn->rstate.cipher_suite = ntohs(cipher);
	offset += 2;

	/* Compression method */
	if (offset + 1 > body_len)
		return -EINVAL;
	/* We only support null compression — ignore non-null for now */

	return body_len;
}

/* ── Handshake State Machine ────────────────────────────────────────── */

/*
 * tls_handshake_step — Drive the TLS handshake one step at a time.
 *
 * For the client role:
 *   state == TLS_HS_IDLE:
 *     builds and returns a ClientHello message.
 *     transitions to TLS_HS_CLIENT_HELLO_SENT.
 *
 *   state == TLS_HS_CLIENT_HELLO_SENT && in != NULL:
 *     expects a ServerHello message.
 *     parses it and transitions to TLS_HS_CONNECTED (basic flow).
 *
 * For the server role:
 *   state == TLS_HS_IDLE && in != NULL:
 *     expects a ClientHello message.
 *     parses it and builds a ServerHello, transitions to
 *     TLS_HS_CONNECTED (basic flow).
 *
 * 'in' / 'in_len' — the raw handshake message body (after the 4-byte
 *   handshake header) received from the peer.  NULL on the first call
 *   for the client.
 *
 * 'out' / 'out_cap' — buffer where the next handshake message to send
 *   will be written as a raw handshake frame (4-byte header + body).
 *
 * Returns the number of bytes written to 'out' (< 0 on error).
 * Sets *new_state to the new handshake state.
 */
int tls_handshake_step(struct tls_conn *conn, enum tls_hs_state *new_state,
                       const uint8_t *in, int in_len,
                       uint8_t *out, int out_cap)
{
	enum tls_hs_state cur_state;
	uint8_t hs_msg_type;
	int hs_body_len;
	int ret;

	if (!conn || !new_state)
		return -EINVAL;
	if (!out || out_cap <= 0)
		return -EINVAL;

	cur_state = *new_state;

	if (conn->is_client) {
		/* ── Client Role ────────────────────────────────── */
		switch (cur_state) {
		case TLS_HS_IDLE:
		{
			/* Supported cipher suites (TLS 1.3 preferred) */
			uint16_t ciphers[] = {
				TLS_AES_128_GCM_SHA256,
				TLS_AES_256_GCM_SHA384,
				TLS_CHACHA20_POLY1305_SHA256,
				TLS_ECDHE_RSA_WITH_AES_128_GCM,
				TLS_ECDHE_RSA_WITH_CHACHA20,
				TLS_DHE_RSA_WITH_AES_128_GCM,
			};
			int num_ciphers = sizeof(ciphers) / sizeof(ciphers[0]);
			uint8_t body[512];
			int body_len;

			body_len = tls_build_client_hello(conn, ciphers,
			                                  num_ciphers,
			                                  NULL, 0,
			                                  body, sizeof(body));
			if (body_len < 0)
				goto error;

			ret = tls_hs_build_frame(TLS_HT_CLIENT_HELLO,
			                         body, body_len,
			                         out, out_cap);
			if (ret < 0)
				goto error;

			*new_state = TLS_HS_CLIENT_HELLO_SENT;
			return ret;
		}

		case TLS_HS_CLIENT_HELLO_SENT:
		{
			if (!in || in_len <= 0)
				return -EINVAL;

			/* Expect a ServerHello */
			ret = tls_hs_parse_header(in, in_len,
			                          &hs_msg_type, &hs_body_len);
			if (ret < 0)
				goto error;

			if (hs_msg_type != TLS_HT_SERVER_HELLO) {
				kprintf("[tls] client: expected ServerHello, "
				        "got %d\n", hs_msg_type);
				goto error;
			}

			if (in_len < TLS_HANDSHAKE_HEADER_LEN + hs_body_len)
				goto error;

			ret = tls_parse_server_hello(
				conn,
				in + TLS_HANDSHAKE_HEADER_LEN,
				hs_body_len);
			if (ret < 0)
				goto error;

			kprintf("[tls] client: received ServerHello, "
			        "cipher 0x%04x\n",
			        conn->rstate.cipher_suite);

			*new_state = TLS_HS_CONNECTED;
			return 0;
		}

		case TLS_HS_CONNECTED:
			/* Handshake complete — nothing to do */
			return 0;

		default:
			goto error;
		}
	} else {
		/* ── Server Role ────────────────────────────────── */
		switch (cur_state) {
		case TLS_HS_IDLE:
		{
			uint16_t cipher_choices[64];
			int num_choices;
			uint16_t selected;
			uint8_t srv_body[512];
			int srv_body_len;

			if (!in || in_len <= 0)
				return -EINVAL;

			/* Expect a ClientHello */
			ret = tls_hs_parse_header(in, in_len,
			                          &hs_msg_type, &hs_body_len);
			if (ret < 0)
				goto error;

			if (hs_msg_type != TLS_HT_CLIENT_HELLO) {
				kprintf("[tls] server: expected ClientHello, "
				        "got %d\n", hs_msg_type);
				goto error;
			}

			if (in_len < TLS_HANDSHAKE_HEADER_LEN + hs_body_len)
				goto error;

			num_choices = 0;

			ret = tls_parse_client_hello(
				conn,
				in + TLS_HANDSHAKE_HEADER_LEN,
				hs_body_len,
				cipher_choices, &num_choices,
				64);
			if (ret < 0)
				goto error;

			/* Negotiate cipher suite using server preference order */
			ret = tls_negotiate_cipher_suite(
				cipher_choices, num_choices, NULL, 0);
			if (ret < 0) {
				kprintf("[tls] server: no mutually supported "
				        "cipher suite\n");
				goto error;
			}
			selected = (uint16_t)ret;

			kprintf("[tls] server: negotiated cipher suite "
			        "0x%04x with client\n", selected);

			/* Build ServerHello */
			srv_body_len = tls_build_server_hello(
				conn, selected,
				conn->session_id,
				conn->session_id_len,
				srv_body, sizeof(srv_body));
			if (srv_body_len < 0)
				goto error;

			ret = tls_hs_build_frame(
				TLS_HT_SERVER_HELLO,
				srv_body, srv_body_len,
				out, out_cap);
			if (ret < 0)
				goto error;

			*new_state = TLS_HS_CONNECTED;
			return ret;
		}

		case TLS_HS_CONNECTED:
			return 0;

		default:
			goto error;
		}
	}

error:
	*new_state = TLS_HS_ERROR;
	return -EPROTO;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

/*
 * tls_handshake_init — Initialise the TLS handshake subsystem.
 *
 * Called from tls_init() after the record layer is set up.
 * Returns 0 on success, negative errno on failure.
 */
int __init tls_handshake_init(void)
{
	kprintf("[ok] tls: handshake protocol initialised (TLS 1.2/1.3)\n");
	return 0;
}

EXPORT_SYMBOL(tls_hs_build_frame);
EXPORT_SYMBOL(tls_hs_parse_header);
EXPORT_SYMBOL(tls_build_client_hello);
EXPORT_SYMBOL(tls_parse_client_hello);
EXPORT_SYMBOL(tls_build_server_hello);
EXPORT_SYMBOL(tls_parse_server_hello);
EXPORT_SYMBOL(tls_handshake_step);

/* ── Cipher Suite Info Table (TLS 1.2 / 1.3) ────────────────────────── */

static const struct tls_cipher_suite_info tls_cipher_suites[] = {
	/* TLS 1.3 cipher suites — AEAD only */
	{
		.cipher_suite  = TLS_AES_128_GCM_SHA256,
		.kx_algo       = TLS_KX_ECDHE,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_AES_128_GCM,
		.mac_algo      = TLS_MAC_AEAD,
		.enc_key_len   = 16,
		.fixed_iv_len  = 4,
		.tag_len       = 16,
		.is_aead       = 1,
	},
	{
		.cipher_suite  = TLS_AES_256_GCM_SHA384,
		.kx_algo       = TLS_KX_ECDHE,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_AES_256_GCM,
		.mac_algo      = TLS_MAC_AEAD,
		.enc_key_len   = 32,
		.fixed_iv_len  = 4,
		.tag_len       = 16,
		.is_aead       = 1,
	},
	{
		.cipher_suite  = TLS_CHACHA20_POLY1305_SHA256,
		.kx_algo       = TLS_KX_ECDHE,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_CHACHA20_POLY1305,
		.mac_algo      = TLS_MAC_AEAD,
		.enc_key_len   = 32,
		.fixed_iv_len  = 4,
		.tag_len       = 16,
		.is_aead       = 1,
	},
	/* TLS 1.2 AEAD cipher suites */
	{
		.cipher_suite  = TLS_ECDHE_ECDSA_WITH_AES_128_GCM,
		.kx_algo       = TLS_KX_ECDHE,
		.auth_algo     = TLS_AUTH_ECDSA,
		.enc_algo      = TLS_ENC_AES_128_GCM,
		.mac_algo      = TLS_MAC_AEAD,
		.enc_key_len   = 16,
		.fixed_iv_len  = 4,
		.tag_len       = 16,
		.is_aead       = 1,
	},
	{
		.cipher_suite  = TLS_ECDHE_RSA_WITH_AES_128_GCM,
		.kx_algo       = TLS_KX_ECDHE,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_AES_128_GCM,
		.mac_algo      = TLS_MAC_AEAD,
		.enc_key_len   = 16,
		.fixed_iv_len  = 4,
		.tag_len       = 16,
		.is_aead       = 1,
	},
	{
		.cipher_suite  = TLS_ECDHE_RSA_WITH_CHACHA20,
		.kx_algo       = TLS_KX_ECDHE,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_CHACHA20_POLY1305,
		.mac_algo      = TLS_MAC_AEAD,
		.enc_key_len   = 32,
		.fixed_iv_len  = 4,
		.tag_len       = 16,
		.is_aead       = 1,
	},
	{
		.cipher_suite  = TLS_DHE_RSA_WITH_AES_128_GCM,
		.kx_algo       = TLS_KX_DHE,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_AES_128_GCM,
		.mac_algo      = TLS_MAC_AEAD,
		.enc_key_len   = 16,
		.fixed_iv_len  = 4,
		.tag_len       = 16,
		.is_aead       = 1,
	},
	/* TLS 1.2 CBC-HMAC cipher suites */
	{
		.cipher_suite  = TLS_RSA_WITH_AES_128_CBC_SHA,
		.kx_algo       = TLS_KX_RSA,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_AES_128_CBC,
		.mac_algo      = TLS_MAC_SHA,
		.enc_key_len   = 16,
		.fixed_iv_len  = 16,
		.tag_len       = 20,   /* SHA-1 HMAC length */
		.is_aead       = 0,
	},
	{
		.cipher_suite  = TLS_RSA_WITH_AES_256_CBC_SHA,
		.kx_algo       = TLS_KX_RSA,
		.auth_algo     = TLS_AUTH_RSA,
		.enc_algo      = TLS_ENC_AES_256_CBC,
		.mac_algo      = TLS_MAC_SHA,
		.enc_key_len   = 32,
		.fixed_iv_len  = 16,
		.tag_len       = 20,
		.is_aead       = 0,
	},
};

#define NUM_CIPHER_SUITES  \
	((int)(sizeof(tls_cipher_suites) / sizeof(tls_cipher_suites[0])))

/* ── Cipher Suite Lookup ──────────────────────────────────────────────── */

const struct tls_cipher_suite_info *
tls_cipher_suite_lookup(uint16_t cipher_suite)
{
	for (int i = 0; i < NUM_CIPHER_SUITES; i++) {
		if (tls_cipher_suites[i].cipher_suite == cipher_suite)
			return &tls_cipher_suites[i];
	}
	return NULL;
}
EXPORT_SYMBOL(tls_cipher_suite_lookup);

/* ── Server Preference Table ─────────────────────────────────────────────
 *
 * Priority-ordered list of cipher suites the server is willing to
 * negotiate.  Ordered from most preferred to least preferred.
 * Forward-secrecy + AEAD suites are ranked highest.
 */

const uint16_t tls_default_server_prefs[] = {
	/* TLS 1.3 (highest security) */
	TLS_AES_128_GCM_SHA256,
	TLS_AES_256_GCM_SHA384,
	TLS_CHACHA20_POLY1305_SHA256,
	/* TLS 1.2 ECDHE + AEAD (forward secrecy) */
	TLS_ECDHE_ECDSA_WITH_AES_128_GCM,
	TLS_ECDHE_RSA_WITH_AES_128_GCM,
	TLS_ECDHE_RSA_WITH_CHACHA20,
	/* TLS 1.2 DHE + AEAD (forward secrecy, no ECC) */
	TLS_DHE_RSA_WITH_AES_128_GCM,
	/* TLS 1.2 RSA + CBC-HMAC (no forward secrecy) */
	TLS_RSA_WITH_AES_128_CBC_SHA,
	TLS_RSA_WITH_AES_256_CBC_SHA,
};

const int tls_default_server_prefs_count =
	(int)(sizeof(tls_default_server_prefs) / sizeof(tls_default_server_prefs[0]));

/* ── Cipher Suite Negotiation ────────────────────────────────────────────
 *
 * tls_negotiate_cipher_suite — Find the best mutually-supported suite.
 *
 * Iterates through the server's preference order and returns the first
 * cipher suite that appears in the client's offered list.  If no match
 * is found, returns -ENOENT.
 *
 * Returns the selected cipher suite (positive uint16_t) on success,
 * or negative errno on failure.
 */
int tls_negotiate_cipher_suite(const uint16_t *client_suites, int num_client,
                               const uint16_t *server_prefs, int num_server)
{
	const uint16_t *prefs;
	int             num_prefs;
	int             i, j;

	if (!client_suites || num_client <= 0)
		return -EINVAL;

	/* Use default server preferences if none supplied */
	if (server_prefs && num_server > 0) {
		prefs     = server_prefs;
		num_prefs = num_server;
	} else {
		prefs     = tls_default_server_prefs;
		num_prefs = tls_default_server_prefs_count;
	}

	for (i = 0; i < num_prefs; i++) {
		uint16_t server_cs = prefs[i];

		for (j = 0; j < num_client; j++) {
			if (client_suites[j] == server_cs)
				return (int)server_cs;
		}
	}

	return -ENOENT;
}
EXPORT_SYMBOL(tls_negotiate_cipher_suite);
