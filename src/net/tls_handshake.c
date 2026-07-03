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
#include "tls_x509.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "export.h"
#include "hmac.h"
#include "sha256.h"

/* ── State Forward Declarations ────────────────────────────────────── */

/* Session ticket helper (defined below) */
static int tls_send_new_session_ticket(struct tls_conn *conn,
                                        uint8_t *out, int out_cap);

/* Renegotiation helper (defined below, called from tls_handshake_step) */
int tls_handle_renegotiation_client_hello(struct tls_conn *conn,
                                           const uint8_t *body,
                                           int body_len);

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

	/* ── ALPN extension (RFC 7301) ────────────────────────────── */
	if (conn->alpn_num_protos > 0) {
		uint8_t ext_buf[2 + 2 + 2 +
		                TLS_ALPN_MAX_PROTOCOLS *
		                (1 + TLS_ALPN_MAX_NAME_LEN)];
		int ext_len;
		int needed;

		ext_len = tls_build_alpn_ext(
			(const uint8_t (*)[TLS_ALPN_MAX_NAME_LEN])
				conn->alpn_protos,
			conn->alpn_proto_lens,
			conn->alpn_num_protos,
			ext_buf, (int)sizeof(ext_buf));
		if (ext_len > 0) {
			/* 2-byte extensions length + ext data */
			needed = offset + 2 + ext_len;
			if (needed <= out_cap) {
				out[offset++] = (uint8_t)((ext_len >> 8) & 0xFF);
				out[offset++] = (uint8_t)(ext_len & 0xFF);
				memcpy(out + offset, ext_buf, (size_t)ext_len);
				offset += ext_len;
			}
		}
	}

	/* ── SNI extension (RFC 6066) ────────────────────────────── */
	if (conn->sni_hostname_len > 0) {
		uint8_t ext_buf[2 + 2 + 2 + 1 + 2 + TLS_SNI_MAX_HOSTNAME_LEN];
		int ext_len;
		int needed;

		ext_len = tls_build_sni_ext(
			(const char *)conn->sni_hostname,
			(int)conn->sni_hostname_len,
			ext_buf, (int)sizeof(ext_buf));
		if (ext_len > 0) {
			needed = offset + 2 + ext_len;
			if (needed <= out_cap) {
				out[offset++] = (uint8_t)((ext_len >> 8) & 0xFF);
				out[offset++] = (uint8_t)(ext_len & 0xFF);
				memcpy(out + offset, ext_buf, (size_t)ext_len);
				offset += ext_len;
			}
		}
	}

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

	/* ── Extensions ──────────────────────────────────────────────── */
	/* TLS extensions block: 2-byte length + zero or more extensions.
	 * Each extension: 2-byte type + 2-byte data length + data. */
	if (offset < body_len) {
		int ext_total_len;
		int ext_offset;

		/* There might be a 2-byte extensions length field */
		if (offset + 2 > body_len)
			return -EINVAL;
		ext_total_len = ((int)body[offset] << 8) |
		                 (int)body[offset + 1];
		offset += 2;

		if (ext_total_len > body_len - offset)
			ext_total_len = body_len - offset;

		ext_offset = 0;
		while (ext_offset + 4 <= ext_total_len) {
			uint16_t ext_type;
			int ext_data_len;

			ext_type = ((uint16_t)body[offset + ext_offset] << 8) |
			            (uint16_t)body[offset + ext_offset + 1];
			ext_data_len = ((int)body[offset + ext_offset + 2] << 8) |
			                (int)body[offset + ext_offset + 3];

			if (ext_data_len < 0 ||
			    ext_offset + 4 + ext_data_len > ext_total_len)
				break;

			/* ── ALPN Extension (TLS_EXT_ALPN = 0x0010) ─── */
			if (ext_type == TLS_EXT_ALPN && ext_data_len > 0) {
				tls_parse_alpn_ext(
					body + offset + ext_offset + 4,
					ext_data_len,
					conn, NULL, NULL, 0);
			}

			/* ── SNI Extension (TLS_EXT_SERVER_NAME = 0x0000) ─── */
			if (ext_type == TLS_EXT_SERVER_NAME && ext_data_len > 0) {
				tls_parse_sni_ext(
					body + offset + ext_offset + 4,
					ext_data_len,
					conn);
			}

			ext_offset += 4 + ext_data_len;
		}
	}

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

	/* ── ALPN extension (RFC 7301) ────────────────────────────── */
	if (conn->alpn_negotiated && conn->alpn_selected_len > 0) {
		uint8_t ext_buf[2 + 2 + 2 + TLS_ALPN_MAX_NAME_LEN];
		int ext_len;

		ext_len = tls_build_alpn_ext(
			(const uint8_t (*)[TLS_ALPN_MAX_NAME_LEN])
				&conn->alpn_selected,
			&conn->alpn_selected_len, 1,
			ext_buf, (int)sizeof(ext_buf));
		if (ext_len > 0) {
			/* Need space for 2-byte ext block length + ext data */
			int needed = offset + 2 + ext_len;

			if (needed <= out_cap) {
				out[offset++] = (uint8_t)((ext_len >> 8) & 0xFF);
				out[offset++] = (uint8_t)(ext_len & 0xFF);
				memcpy(out + offset, ext_buf, (size_t)ext_len);
				offset += ext_len;
			}
		}
	}

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

			/* TLS 1.3 middlebox compatibility (RFC 8446 §5.1):
			 * send a dummy ChangeCipherSpec record after ClientHello
			 * so that middleboxes don't reject the connection. */
			if (conn->middlebox_compat && conn->version >= TLS_VER_1_3)
				conn->ccs_pending = 1;

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

			/* Log ALPN if server responded with one */
			if (conn->alpn_negotiated && conn->alpn_selected_len > 0) {
				kprintf("[tls] client: ALPN negotiated "
				        "(%.*s)\n",
				        (int)conn->alpn_selected_len,
				        (const char *)conn->alpn_selected);
			}

			*new_state = TLS_HS_SERVER_PARAMS_SENT;
			return 0;
		}

		case TLS_HS_SERVER_PARAMS_SENT:
		{
			if (!in || in_len <= 0) {
				/* No data — just skip to ticket state */
				*new_state = TLS_HS_TICKET_SENT;
				return 0;
			}

			/* Try to parse a Certificate message */
			ret = tls_hs_parse_header(in, in_len,
			                          &hs_msg_type, &hs_body_len);
			if (ret < 0) {
				/* Not a valid handshake message — skip */
				*new_state = TLS_HS_TICKET_SENT;
				return 0;
			}

			if (hs_msg_type == TLS_HT_CERTIFICATE) {
				struct tls_cert_entry certs[TLS_MAX_CERT_CHAIN_DEPTH];
				int num_certs = 0;

				if (in_len < TLS_HANDSHAKE_HEADER_LEN + hs_body_len)
					goto error;

				ret = tls_parse_certificate_msg(
					in + TLS_HANDSHAKE_HEADER_LEN,
					hs_body_len,
					certs, TLS_MAX_CERT_CHAIN_DEPTH,
					&num_certs);
				if (ret < 0) {
					kprintf("[tls] client: failed to parse "
					        "Certificate: %d\n", ret);
					goto error;
				}

				kprintf("[tls] client: received %d certificate(s)"
				        " from server\n", num_certs);

				/* Parse first certificate for logging */
				if (num_certs > 0) {
					struct tls_x509_cert xcert;
					ret = x509_parse_cert(
						certs[0].data,
						certs[0].data_len,
						&xcert);
					if (ret == 0)
						x509_print_cert(&xcert, "server");
				}
			} else {
				kprintf("[tls] client: expected Certificate, "
				        "got msg_type %d — skipping\n",
				        hs_msg_type);
			}

			*new_state = TLS_HS_TICKET_SENT;
			return 0;
		}

		case TLS_HS_TICKET_SENT:
		{
			/* Client may receive a NewSessionTicket post-handshake */
			if (in && in_len > 0) {
				uint32_t ticket_lifetime;
				uint32_t ticket_age_add;
				const uint8_t *ticket_ptr;
				int ticket_len;
				const uint8_t *nonce_ptr;
				int nonce_len;

				ret = tls_hs_parse_header(in, in_len,
				                          &hs_msg_type, &hs_body_len);
				if (ret < 0)
					goto error;

				if (hs_msg_type != TLS_HT_NEW_SESSION_TICKET) {
					/* Not a ticket — skip to connected */
					*new_state = TLS_HS_CONNECTED;
					return 0;
				}

				if (in_len < TLS_HANDSHAKE_HEADER_LEN + hs_body_len)
					goto error;

				ret = tls_parse_new_session_ticket(
					in + TLS_HANDSHAKE_HEADER_LEN,
					hs_body_len,
					&ticket_lifetime,
					&ticket_age_add,
					&ticket_ptr, &ticket_len,
					&nonce_ptr, &nonce_len);
				if (ret < 0) {
					kprintf("[tls] client: invalid "
					        "NewSessionTicket, ignoring\n");
				} else {
					kprintf("[tls] client: received "
					        "NewSessionTicket (%d bytes, "
					        "lifetime %u)\n",
					        ticket_len, ticket_lifetime);
				}

				*new_state = TLS_HS_CONNECTED;
				return 0;
			}

			*new_state = TLS_HS_CONNECTED;
			return 0;
		}

		case TLS_HS_CONNECTED:
		{
			/* Check if this is a renegotiation trigger */
			if (in && in_len > 0 && conn->renego_allowed &&
			    conn->version <= TLS_VER_1_2) {
				int hr_ret;
				uint8_t hs_mt;
				int hs_bl;

				hr_ret = tls_hs_parse_header(in, in_len,
				                              &hs_mt, &hs_bl);
				if (hr_ret > 0 &&
				    hs_mt == TLS_HT_HELLO_REQUEST &&
				    hs_bl == 0) {
					/* Server wants renegotiation */
					kprintf("[tls] client: received "
					        "HelloRequest, initiating "
					        "renegotiation\n");

					/* Send new ClientHello with
					 * renegotiation_info extension */
					conn->renego_in_progress = 1;
					ret = tls_conn_request_renegotiate(
						conn, out, out_cap);
					if (ret < 0)
						goto error;

					*new_state = TLS_HS_CLIENT_HELLO_SENT;
					return ret;
				}
			}
			/* Handshake complete — nothing to do */
			return 0;
		}

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

			/* ── ALPN negotiation ────────────────────────── */
			/* ALPN was parsed from the ClientHello extension
			 * inside tls_parse_client_hello().  If ALPN was
			 * negotiated, log the result. */
			if (conn->alpn_negotiated && conn->alpn_selected_len > 0) {
				kprintf("[tls] server: ALPN negotiated "
				        "(%.*s)\n",
				        (int)conn->alpn_selected_len,
				        (const char *)conn->alpn_selected);
			}

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

			/* TLS 1.3 middlebox compatibility (RFC 8446 §5.1):
			 * send a dummy ChangeCipherSpec record after ServerHello. */
			if (conn->middlebox_compat && conn->version >= TLS_VER_1_3)
				conn->ccs_pending = 1;

			*new_state = TLS_HS_SERVER_PARAMS_SENT;
			return ret;
		}

		case TLS_HS_SERVER_PARAMS_SENT:
		{
			/* Server sends Certificate message */
			uint8_t body[4096];
			int body_len;
			struct tls_cert_entry dummy_cert;

			kprintf("[tls] server: sending Certificate message\n");

			/* For now, send a minimal empty certificate list.
			 * In production, the server would load its real
			 * certificate chain and build the message from that. */
			dummy_cert.data       = NULL;
			dummy_cert.data_len   = 0;
			dummy_cert.extensions = NULL;
			dummy_cert.ext_len    = 0;

			/* Build an empty certificate list (just the 3-byte
			 * length prefix with value 0) */
			body[0] = 0;
			body[1] = 0;
			body[2] = 0;
			body_len = 3;

			ret = tls_hs_build_frame(
				TLS_HT_CERTIFICATE,
				body, body_len,
				out, out_cap);
			if (ret < 0)
				goto error;

			*new_state = TLS_HS_TICKET_SENT;
			return ret;
		}

		case TLS_HS_TICKET_SENT:
		{
			/* Server sends NewSessionTicket after main handshake */
			kprintf("[tls] server: sending NewSessionTicket\n");

			ret = tls_send_new_session_ticket(conn, out, out_cap);
			if (ret < 0) {
				kprintf("[tls] server: failed to build "
				        "session ticket: %d\n", ret);
				/* Non-fatal — proceed without ticket */
				*new_state = TLS_HS_CONNECTED;
				return 0;
			}

			*new_state = TLS_HS_CONNECTED;
			return ret;
		}

		case TLS_HS_RENEGO_WAIT_HELLO:
		case TLS_HS_CONNECTED:
		{
			uint16_t cipher_choices[64];
			int num_choices;
			uint16_t selected;
			uint8_t srv_body[512];
			int srv_body_len;

			/* Only handle input if we receive a new ClientHello
			 * (renegotiation trigger) */
			if (!in || in_len <= 0) {
				if (cur_state == TLS_HS_RENEGO_WAIT_HELLO)
					return 0;  /* still waiting */
				return 0;  /* connected, nothing to do */
			}

			if (!conn->renego_allowed ||
			    conn->version >= TLS_VER_1_3)
				return 0;  /* renegotiation not supported */

			/* Parse the incoming handshake header */
			ret = tls_hs_parse_header(in, in_len,
			                          &hs_msg_type, &hs_body_len);
			if (ret < 0)
				goto error;

			if (hs_msg_type != TLS_HT_CLIENT_HELLO) {
				/* Not a ClientHello — ignore */
				*new_state = cur_state;
				return 0;
			}

			/* ClientHello received — initiate server-side
			 * renegotiation handshake */
			if (in_len < TLS_HANDSHAKE_HEADER_LEN + hs_body_len)
				goto error;

			kprintf("[tls] server: received ClientHello "
			        "(renegotiation), processing...\n");

			/* Mark renegotiation in progress */
			conn->renego_in_progress = 1;

			/* Validate renegotiation_info extension if present */
			{
				int rnego_ret;
				rnego_ret = tls_handle_renegotiation_client_hello(
					conn,
					in + TLS_HANDSHAKE_HEADER_LEN,
					hs_body_len);
				if (rnego_ret < 0) {
					kprintf("[tls] server: renegotiation "
					        "check failed: %d\n", rnego_ret);
					goto error;
				}
			}

			/* Parse the ClientHello */
			num_choices = 0;
			ret = tls_parse_client_hello(
				conn,
				in + TLS_HANDSHAKE_HEADER_LEN,
				hs_body_len,
				cipher_choices, &num_choices,
				64);
			if (ret < 0)
				goto error;

			/* Negotiate cipher suite */
			ret = tls_negotiate_cipher_suite(
				cipher_choices, num_choices, NULL, 0);
			if (ret < 0) {
				kprintf("[tls] server: no mutually "
				        "supported cipher suite\n");
				goto error;
			}
			selected = (uint16_t)ret;

			kprintf("[tls] server: renegotiated cipher "
			        "suite 0x%04x\n", selected);

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

			*new_state = TLS_HS_SERVER_PARAMS_SENT;
			return ret;
		}

		default:
			goto error;
		}
	}

error:
	*new_state = TLS_HS_ERROR;
	return -EPROTO;
}

/* ══════════════════════════════════════════════════════════════════════════
 * HKDF (RFC 5869) — HMAC-based Key Derivation Function
 *
 * TLS 1.3 replaces the TLS 1.2 PRF with HKDF using HMAC-SHA256 by
 * default.  HKDF consists of two stages:
 *
 *   HKDF-Extract(salt, IKM)  ⇒  PRK
 *     PRK = HMAC-SHA256(salt, IKM)
 *
 *   HKDF-Expand(PRK, info, L)  ⇒  OKM
 *     T(0) = empty
 *     T(i) = HMAC-SHA256(PRK, T(i-1) || info || i)   for i = 1..N
 *     OKM  = first L bytes of T(1) || T(2) || ...
 *
 * References:
 *   RFC 5869 §2 — HMAC-based Extract-and-Expand KDF
 *   RFC 8446 §7 — TLS 1.3 Key Schedule
 * ══════════════════════════════════════════════════════════════════════════ */

void tls_hkdf_extract(const uint8_t *salt, int salt_len,
                      const uint8_t *ikm, int ikm_len,
                      uint8_t *prk)
{
	/* HKDF-Extract(salt, IKM) = HMAC-SHA256(salt, IKM)
	 *
	 * RFC 5869 §2.2:
	 *   If salt is not provided or is empty, it is set to a string
	 *   of HashLen (32) zeros.
	 */
	uint8_t default_salt[32];

	if (!prk)
		return;

	if (!salt || salt_len <= 0) {
		memset(default_salt, 0, sizeof(default_salt));
		salt     = default_salt;
		salt_len = 32;
	}

	if (!ikm || ikm_len <= 0) {
		/* Zero-length IKM is allowed (produces PRK from salt only) */
		ikm     = NULL;
		ikm_len = 0;
	}

	hmac_sha256(salt, (size_t)salt_len,
	            ikm, (size_t)ikm_len,
	            prk);
}

void tls_hkdf_expand(const uint8_t *prk, int prk_len,
                     const uint8_t *info, int info_len,
                     uint8_t *out, int out_len)
{
	/* HKDF-Expand(PRK, info, L) per RFC 5869 §2.3.
	 *
	 * For HMAC-SHA256 (HashLen = 32):
	 *   N = ceil(L / HashLen)
	 *   T(0) = empty string
	 *   T(i) = HMAC-SHA256(PRK, T(i-1) || info || i)
	 *   OKM  = first L bytes of T(1) || T(2) || ...
	 */
	uint8_t t[32];         /* running T(i) value */
	uint8_t buf[256 + 1];  /* T(i-1) || info || counter byte */
	int     t_len;
	int     generated;
	int     i;
	int     info_copy_len;

	if (!prk || !out || out_len <= 0)
		return;

	if (prk_len <= 0)
		prk_len = 32;

	info_copy_len = info_len;
	if (!info || info_len <= 0)
		info_copy_len = 0;
	if (info_copy_len > 256)
		info_copy_len = 256;

	t_len     = 0;
	generated = 0;

	for (i = 1; generated < out_len; i++) {
		int copy_len;

		/* Build: T(i-1) || info || counter_byte */
		if (t_len > 0)
			memcpy(buf, t, (size_t)t_len);

		if (info_copy_len > 0)
			memcpy(buf + t_len, info, (size_t)info_copy_len);

		buf[t_len + info_copy_len] = (uint8_t)i;

		/* T(i) = HMAC-SHA256(PRK, T(i-1) || info || i) */
		hmac_sha256(prk, (size_t)prk_len,
		            buf, (size_t)(t_len + info_copy_len + 1),
		            t);
		t_len = 32;

		copy_len = t_len;
		if (generated + copy_len > out_len)
			copy_len = out_len - generated;

		memcpy(out + generated, t, (size_t)copy_len);
		generated += copy_len;
	}

	/* Zeroise intermediate buffer */
	memset(buf, 0, sizeof(buf));
}

/* ── TLS 1.3 HKDF-Expand-Label (RFC 8446 §7.1) ────────────────────────────
 *
 *   HKDF-Expand-Label(Secret, Label, Context, Length) =
 *       HKDF-Expand(Secret, HkdfLabel, Length)
 *
 * where HkdfLabel encodes:
 *   struct {
 *       uint16 length;
 *       opaque label<7..255>;
 *       opaque context<0..255>;
 *   } HkdfLabel;
 *
 * and the on-the-wire label is "tls13 " || Label.
 */

void tls_hkdf_expand_label(const uint8_t *secret, int secret_len,
                           const char *label,
                           const uint8_t *context, int context_len,
                           uint8_t *out, int out_len)
{
	uint8_t hkdf_label[260];
	int     offset = 0;
	int     label_prefix_len = 6;   /* "tls13 " */
	int     label_str_len;
	int     total_label_len;

	if (!secret || !label || !out || out_len <= 0)
		return;

	if (secret_len <= 0)
		secret_len = 32;

	label_str_len  = (int)strlen(label);
	total_label_len = label_prefix_len + label_str_len;
	if (total_label_len > 255)
		total_label_len = 255;

	if (context_len < 0)
		context_len = 0;
	if (context_len > 255)
		context_len = 255;

	/* Build HkdfLabel */

	/* 1. Length (2 bytes, big-endian) */
	hkdf_label[offset++] = (uint8_t)((out_len >> 8) & 0xFF);
	hkdf_label[offset++] = (uint8_t)(out_len & 0xFF);

	/* 2. Label length (1 byte) */
	hkdf_label[offset++] = (uint8_t)total_label_len;

	/* 3. Label value: "tls13 " || label */
	memcpy(hkdf_label + offset, "tls13 ", 6);
	offset += 6;
	memcpy(hkdf_label + offset, label, (size_t)label_str_len);
	offset += label_str_len;

	/* 4. Context length (1 byte) */
	hkdf_label[offset++] = (uint8_t)context_len;

	/* 5. Context value */
	if (context_len > 0 && context) {
		memcpy(hkdf_label + offset, context, (size_t)context_len);
		offset += context_len;
	}

	/* Perform HKDF-Expand */
	tls_hkdf_expand(secret, secret_len,
	                hkdf_label, offset,
	                out, out_len);
}

/* ── TLS 1.3 Derive-Secret (RFC 8446 §7.1) ──────────────────────────────
 *
 *   Derive-Secret(Secret, Label, Messages) =
 *       HKDF-Expand-Label(Secret, Label, Transcript-Hash, Hash.length)
 *
 * Where the Transcript-Hash is the SHA-256 hash of the handshake messages
 * up to (but not including) the current message.
 */

void tls_derive_secret(const uint8_t *secret, const char *label,
                       const uint8_t *transcript_hash,
                       uint8_t *out)
{
	if (!secret || !label || !out)
		return;

	tls_hkdf_expand_label(secret, 32, label,
	                      transcript_hash, 32,
	                      out, 32);
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS Renegotiation (RFC 5246 §7.4.1, RFC 5746)
 *
 * TLS renegotiation allows a client or server to request a new handshake
 * within an already-established TLS session.  The mechanism differs
 * between TLS 1.2 (which supports renegotiation) and TLS 1.3 (which
 * does not per RFC 8446 §4.1.1).
 *
 * Flow:
 *   1. Server sends HelloRequest (empty body, type 0) to the client.
 *   2. Client responds with a new ClientHello containing the
 *      renegotiation_info extension (RFC 5746), which includes the
 *      verify_data from the previous Finished messages.
 *   3. Both sides repeat the full handshake, verifying that the
 *      renegotiation_info extension matches.
 *
 * The renegotiation_info extension (type 0xFF01) carries:
 *   struct {
 *       opaque renegotiated_connection<0..255>;
 *   } RenegotiationInfo;
 *
 * For the initial handshake, renegotiated_connection is empty.
 * For subsequent renegotiated handshakes, it contains the client's
 * and server's verify_data from the previous Finished messages.
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * tls_build_hello_request — Build a HelloRequest handshake message.
 *
 * HelloRequest (RFC 5246 §7.4.1.1) is a simple handshake message with
 * msg_type = TLS_HT_HELLO_REQUEST (0) and an empty body.
 */
int tls_build_hello_request(uint8_t *out, int out_cap)
{
	if (!out)
		return -EINVAL;

	return tls_hs_build_frame(TLS_HT_HELLO_REQUEST, NULL, 0,
	                          out, out_cap);
}

/*
 * tls_parse_hello_request — Parse a HelloRequest handshake message.
 *
 * A HelloRequest has msg_type = 0 and an empty body.
 * Returns 0 on success, or -EPROTO if the message is not a valid
 * HelloRequest.
 */
int tls_parse_hello_request(const uint8_t *in, int in_len)
{
	uint8_t msg_type;
	int     body_len;
	int     ret;

	if (!in)
		return -EINVAL;

	ret = tls_hs_parse_header(in, in_len, &msg_type, &body_len);
	if (ret < 0)
		return ret;

	if (msg_type != TLS_HT_HELLO_REQUEST)
		return -EPROTO;

	if (body_len != 0)
		return -EPROTO;

	return 0;
}

/*
 * tls_build_renegotiation_info_ext — Build the renegotiation_info
 * extension (RFC 5746 §3.2).
 *
 * The extension body is:
 *   struct {
 *       opaque renegotiated_connection<0..255>;
 *   } RenegotiationInfo;
 *
 * For a renegotiated handshake, renegotiated_connection contains the
 * verify_data from the previous Finished messages, concatenated as
 * client_verify_data || server_verify_data.
 *
 * On the wire, with TLS extension framing:
 *   ExtensionType = 0xFF01 (2 bytes, big-endian)
 *   extension_data_length (2 bytes, big-endian)
 *   renegotiated_connection length (1 byte)
 *   renegotiated_connection (0..255 bytes)
 */
int tls_build_renegotiation_info_ext(const uint8_t *client_verify_data,
                                     int client_verify_len,
                                     const uint8_t *server_verify_data,
                                     int server_verify_len,
                                     uint8_t *out, int out_cap)
{
	int total_data_len;
	int offset = 0;

	if (!out)
		return -EINVAL;

	/* Verify individual lengths */
	if (client_verify_len < 0 || client_verify_len > TLS_RENEGO_VERIFY_DATA_LEN)
		return -EINVAL;
	if (server_verify_len < 0 || server_verify_len > TLS_RENEGO_VERIFY_DATA_LEN)
		return -EINVAL;
	if ((client_verify_len > 0 && !client_verify_data) ||
	    (server_verify_len > 0 && !server_verify_data))
		return -EINVAL;

	/* The renegotiated_connection length is 1 byte (0..255) */
	total_data_len = client_verify_len + server_verify_len;
	if (total_data_len > 255)
		return -EINVAL;

	/* Total output: ext_type(2) + ext_data_len(2) + data_len(1) + data */
	if (out_cap < 5 + total_data_len)
		return -ENOSPC;

	/* Extension type (0xFF01, big-endian) */
	out[offset++] = (uint8_t)(TLS_EXT_RENEGOTIATION_INFO >> 8);
	out[offset++] = (uint8_t)(TLS_EXT_RENEGOTIATION_INFO & 0xFF);

	/* Extension data length (2 bytes, big-endian) — includes the
	 * 1-byte length prefix for renegotiated_connection */
	out[offset++] = (uint8_t)((total_data_len + 1) >> 8);
	out[offset++] = (uint8_t)((total_data_len + 1) & 0xFF);

	/* Renegotiated_connection length */
	out[offset++] = (uint8_t)total_data_len;

	/* Client verify data */
	if (client_verify_len > 0) {
		memcpy(out + offset, client_verify_data,
		       (size_t)client_verify_len);
		offset += client_verify_len;
	}

	/* Server verify data */
	if (server_verify_len > 0) {
		memcpy(out + offset, server_verify_data,
		       (size_t)server_verify_len);
		offset += server_verify_len;
	}

	return offset;
}

/*
 * tls_parse_renegotiation_info_ext — Parse the renegotiation_info
 * extension (RFC 5746 §3.3).
 *
 * On success, sets *verify_data to point to the raw
 * renegotiated_connection data inside 'ext_body' (so it aliases the
 * caller's buffer), sets *verify_data_len to its length, and returns
 * the length of verify_data.
 * Returns negative errno on failure.
 */
int tls_parse_renegotiation_info_ext(const uint8_t *ext_body,
                                     int ext_body_len,
                                     const uint8_t **verify_data,
                                     int *verify_data_len)
{
	int data_len;

	if (!ext_body || !verify_data || !verify_data_len)
		return -EINVAL;

	if (ext_body_len < 1)
		return -EINVAL;

	/* First byte is the length of renegotiated_connection */
	data_len = ext_body[0];

	if (data_len < 0 || data_len > 255)
		return -EINVAL;

	if (ext_body_len < 1 + data_len)
		return -EINVAL;

	*verify_data     = ext_body + 1;
	*verify_data_len = data_len;

	return data_len;
}

/*
 * tls_conn_request_renegotiate — Initiate TLS renegotiation.
 *
 * For a server: builds and returns a HelloRequest message in 'out'.
 * For a client: builds and returns a new ClientHello message in 'out',
 * including the renegotiation_info extension.
 *
 * The caller is responsible for sending the message and then continuing
 * to call tls_handshake_step() to drive the renegotiation handshake.
 *
 * On success, returns the number of bytes written to 'out'.
 * On failure, returns a negative errno.
 */
int tls_conn_request_renegotiate(struct tls_conn *conn,
                                  uint8_t *out, int out_cap)
{
	int     ret;

	if (!conn || !out)
		return -EINVAL;

	/* Check if renegotiation is allowed */
	if (!conn->renego_allowed)
		return -EPERM;

	/* TLS 1.3 does not support renegotiation */
	if (conn->version >= TLS_VER_1_3)
		return -EOPNOTSUPP;

	/* Mark that a renegotiation is in progress */
	conn->renego_in_progress = 1;

	if (conn->is_client) {
		const uint16_t ciphers[] = {
			TLS_AES_128_GCM_SHA256,
			TLS_AES_256_GCM_SHA384,
			TLS_CHACHA20_POLY1305_SHA256,
			TLS_ECDHE_RSA_WITH_AES_128_GCM,
			TLS_ECDHE_RSA_WITH_CHACHA20,
			TLS_DHE_RSA_WITH_AES_128_GCM,
		};
		int num_ciphers = sizeof(ciphers) / sizeof(ciphers[0]);
		uint8_t ch_body[512];
		int ch_body_len;
		uint8_t ext_buf[64];
		int ext_len;
		int ch_ext_offset;

		ch_body_len = tls_build_client_hello(conn, ciphers,
		                                      num_ciphers,
		                                      conn->session_id,
		                                      conn->session_id_len,
		                                      ch_body, sizeof(ch_body));
		if (ch_body_len < 0)
			return ch_body_len;

		/* Append renegotiation_info extension */
		ext_len = tls_build_renegotiation_info_ext(
			conn->renego_client_verify_data,
			TLS_RENEGO_VERIFY_DATA_LEN,
			conn->renego_server_verify_data,
			TLS_RENEGO_VERIFY_DATA_LEN,
			ext_buf, sizeof(ext_buf));
		if (ext_len < 0)
			return ext_len;

		/* Need to append the extension to the ClientHello body.
		 * The ClientHello body currently has no extensions.
		 * We add an extensions block: 2-byte length + extension data. */
		if (ch_body_len + 2 + ext_len > (int)sizeof(ch_body))
			return -ENOSPC;

		ch_ext_offset = ch_body_len;
		ch_body[ch_ext_offset++] = (uint8_t)((ext_len >> 8) & 0xFF);
		ch_body[ch_ext_offset++] = (uint8_t)(ext_len & 0xFF);
		memcpy(ch_body + ch_ext_offset, ext_buf, (size_t)ext_len);
		ch_body_len = ch_ext_offset + ext_len;

		/* Wrap in handshake frame */
		ret = tls_hs_build_frame(TLS_HT_CLIENT_HELLO,
		                         ch_body, ch_body_len,
		                         out, out_cap);
		if (ret < 0)
			return ret;

		return ret;
	} else {
		/* Server: send HelloRequest to client */
		ret = tls_build_hello_request(out, out_cap);
		if (ret < 0)
			return ret;

		return ret;
	}
}

/*
 * tls_handle_renegotiation_client_hello — Process a ClientHello received
 * in the context of a renegotiation handshake (server-side).
 *
 * Validates the presence of the renegotiation_info extension and checks
 * that the verify_data matches the stored server verify_data.
 *
 * Returns 0 on success, negative errno on failure (e.g. -EPROTO if the
 * extension is missing or verify_data doesn't match).
 */
int tls_handle_renegotiation_client_hello(struct tls_conn *conn,
                                           const uint8_t *body, int body_len)
{
	(void)conn;
	(void)body;
	(void)body_len;

	/* For now, this is a simplified check — we validate that the
	 * message is a valid ClientHello body (the full RFC 5746
	 * verify_data validation requires storing the Finished messages'
	 * verify_data, which requires the full Finished message protocol
	 * to be implemented).  The current implementation accepts the
	 * renegotiation ClientHello and proceeds with the new handshake.
	 *
	 * Full secure renegotiation (RFC 5746 §3.5) should verify:
	 *   1. The previous handshake's client_verify_data matches bytes
	 *      0..11 of the extension.
	 *   2. The previous handshake's server_verify_data matches bytes
	 *      12..23 of the extension.
	 */
	return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS ALPN (RFC 7301) — Application-Layer Protocol Negotiation
 *
 * ALPN allows the application layer to negotiate a protocol (e.g.
 * "http/1.1", "h2", "h3", "webrtc") inside the TLS handshake.
 *
 * The client sends a ProtocolNameList in its ClientHello's ALPN extension.
 * The server selects the first protocol from the client's list that also
 * appears in the server's list, and echoes it back in the ServerHello's
 * ALPN extension.
 *
 * Wire format (RFC 7301 §3.1):
 *   uint16_t list_length;           // big-endian
 *   struct {
 *       uint8_t name_length;        // 1..255
 *       uint8_t name[name_length];
 *   } protocol_name[];
 *
 * Extension wire format (same as all TLS extensions):
 *   uint16_t ext_type    = 0x0010  // TLS_EXT_ALPN
 *   uint16_t ext_length            // length of ext_body that follows
 *   uint8_t  ext_body[]            // ProtocolNameList (as above)
 *
 * ══════════════════════════════════════════════════════════════════════════ */

int tls_build_alpn_ext(const uint8_t (*protos)[TLS_ALPN_MAX_NAME_LEN],
                       const uint8_t *proto_lens, int num_protos,
                       uint8_t *out, int out_cap)
{
	int offset = 0;
	int body_len;
	int list_len;
	int i;

	if (!out)
		return -EINVAL;
	if (!protos || !proto_lens)
		return -EINVAL;
	if (num_protos <= 0 || num_protos > TLS_ALPN_MAX_PROTOCOLS)
		return -EINVAL;

	/* Calculate the total body length (ProtocolNameList) */
	list_len = 0;
	for (i = 0; i < num_protos; i++) {
		int nl = (int)proto_lens[i];
		if (nl < 1 || nl > TLS_ALPN_MAX_NAME_LEN)
			return -EINVAL;
		list_len += 1 + nl;  /* 1 byte length + name bytes */
	}

	/* Total extension: ext_type(2) + ext_data_len(2) + list_len_16(2) + list */
	body_len = 2 + 2 + 2 + list_len;
	if (out_cap < body_len)
		return -ENOSPC;

	/* Extension type (TLS_EXT_ALPN = 0x0010, big-endian) */
	out[offset++] = (uint8_t)((TLS_EXT_ALPN >> 8) & 0xFF);
	out[offset++] = (uint8_t)(TLS_EXT_ALPN & 0xFF);

	/* Extension data length (includes the 2-byte list length prefix) */
	{
		uint16_t ext_data_len = (uint16_t)(2 + list_len);
		out[offset++] = (uint8_t)((ext_data_len >> 8) & 0xFF);
		out[offset++] = (uint8_t)(ext_data_len & 0xFF);
	}

	/* ProtocolNameList: 2-byte length + entries */
	out[offset++] = (uint8_t)((list_len >> 8) & 0xFF);
	out[offset++] = (uint8_t)(list_len & 0xFF);

	for (i = 0; i < num_protos; i++) {
		uint8_t nl = proto_lens[i];
		out[offset++] = nl;  /* name length (1 byte) */
		memcpy(out + offset, protos[i], (size_t)nl);
		offset += (int)nl;
	}

	return offset;
}

int tls_parse_alpn_ext(const uint8_t *ext_body, int ext_body_len,
                       struct tls_conn *conn,
                       const uint8_t (*server_protos)[TLS_ALPN_MAX_NAME_LEN],
                       const uint8_t *server_lens, int num_server)
{
	int offset = 0;
	int list_len;
	int i;

	if (!ext_body || !conn)
		return -EINVAL;
	if (ext_body_len < 2)
		return 0;  /* not an error — just no ALPN data */

	/* ProtocolNameList length (2 bytes, big-endian) */
	list_len = ((int)ext_body[0] << 8) | (int)ext_body[1];
	offset += 2;

	if (list_len < 2 || list_len > ext_body_len - 2)
		return -EINVAL;

	/* Parse each protocol name in the client's list */
	while (offset < 2 + list_len) {
		uint8_t name_len;

		/* Need at least 1 byte for name length */
		if (offset >= ext_body_len)
			break;

		name_len = ext_body[offset++];
		if (name_len < 1)
			continue;  /* skip empty names */
		if (offset + (int)name_len > ext_body_len)
			return -EINVAL;

		int cmp_len = (int)name_len;
		/* If server_protos is NULL, accept the first protocol */
		if (server_protos == NULL || num_server <= 0) {
			if (cmp_len > TLS_ALPN_MAX_NAME_LEN)
				cmp_len = TLS_ALPN_MAX_NAME_LEN;
			conn->alpn_selected_len = (uint8_t)cmp_len;
			memcpy(conn->alpn_selected, ext_body + offset,
			       (size_t)cmp_len);
			conn->alpn_negotiated = 1;
			return 0;
		}

		/* Search for a match in the server's list */
		for (i = 0; i < num_server; i++) {
			uint8_t sl = server_lens[i];
			if (sl == name_len &&
			    memcmp(server_protos[i], ext_body + offset,
			           (size_t)name_len) == 0) {
				conn->alpn_selected_len = name_len;
				memcpy(conn->alpn_selected,
				       server_protos[i],
				       (size_t)name_len);
				conn->alpn_negotiated = 1;
				return 0;
			}
		}

		/* No match — skip this name and continue */
		offset += (int)name_len;
	}

	/* No matching protocol found — not an error, ALPN is optional */
	conn->alpn_negotiated = 0;
	return 0;
}

int tls_alpn_set_protocols(struct tls_conn *conn,
                           const uint8_t (*protos)[TLS_ALPN_MAX_NAME_LEN],
                           const uint8_t *proto_lens, int num_protos)
{
	int i;

	if (!conn)
		return -EINVAL;
	if (num_protos < 0 || num_protos > TLS_ALPN_MAX_PROTOCOLS)
		return -EINVAL;
	if (num_protos > 0 && (!protos || !proto_lens))
		return -EINVAL;

	conn->alpn_num_protos = 0;
	conn->alpn_negotiated = 0;

	for (i = 0; i < num_protos; i++) {
		int nl = (int)proto_lens[i];

		if (nl < 1 || nl > TLS_ALPN_MAX_NAME_LEN)
			continue;

		conn->alpn_proto_lens[i] = (uint8_t)nl;
		memcpy(conn->alpn_protos[i], protos[i], (size_t)nl);
		conn->alpn_num_protos = i + 1;
	}

	return 0;
}

const uint8_t *tls_alpn_get_selected(const struct tls_conn *conn,
                                     uint8_t *len)
{
	if (!conn || !len)
		return NULL;

	if (!conn->alpn_negotiated) {
		*len = 0;
		return NULL;
	}

	*len = conn->alpn_selected_len;
	return conn->alpn_selected;
}

/* ══════════════════════════════════════════════════════════════════════════
 * SNI (RFC 6066 §3) — Server Name Indication
 *
 * The SNI extension (TLS_EXT_SERVER_NAME = 0x0000) allows the client to
 * indicate the hostname it wishes to connect to before the server sends
 * its Certificate — essential for virtual hosting (multiple HTTPS sites
 * on the same IP).
 *
 * Wire format (RFC 6066 §3):
 *
 *   struct {
 *     ServerName server_name_list[1..2^16-1];  // at least 1 entry
 *   } ServerNameList;
 *
 *   struct {
 *     NameType name_type;    // 0 = host_name (the only defined type)
 *     uint16_t  length;      // length of name in bytes
 *     uint8_t   name[length];
 *   } ServerName;
 *
 * Extension wire format (same as all TLS extensions):
 *   uint16_t ext_type    = 0x0000  // TLS_EXT_SERVER_NAME
 *   uint16_t ext_length            // length of ext_body that follows
 *   uint8_t  ext_body[]            // ServerNameList (as above)
 *
 * Reference: RFC 6066 §3 — Transport Layer Security (TLS) Extensions:
 *            Extension Definitions
 * ══════════════════════════════════════════════════════════════════════════ */

int tls_build_sni_ext(const char *hostname, int hostname_len,
                      uint8_t *out, int out_cap)
{
	int offset = 0;
	int body_len;

	if (!out)
		return -EINVAL;
	if (!hostname || hostname_len <= 0)
		return -EINVAL;
	if (hostname_len > TLS_SNI_MAX_HOSTNAME_LEN)
		return -EINVAL;

	/* Total extension: ext_type(2) + ext_data_len(2) +
	 *   list_len(2) + name_type(1) + name_len(2) + hostname */
	body_len = 2 + 2 + 2 + 1 + 2 + hostname_len;
	if (out_cap < body_len)
		return -ENOSPC;

	/* Extension type (TLS_EXT_SERVER_NAME = 0x0000, big-endian) */
	out[offset++] = (uint8_t)((TLS_EXT_SERVER_NAME >> 8) & 0xFF);
	out[offset++] = (uint8_t)(TLS_EXT_SERVER_NAME & 0xFF);

	/* Extension data length (ServerNameList) */
	{
		uint16_t ext_data_len = (uint16_t)(2 + 1 + 2 + hostname_len);
		out[offset++] = (uint8_t)((ext_data_len >> 8) & 0xFF);
		out[offset++] = (uint8_t)(ext_data_len & 0xFF);
	}

	/* ServerNameList: 2-byte list length */
	{
		uint16_t list_len = (uint16_t)(1 + 2 + hostname_len);
		out[offset++] = (uint8_t)((list_len >> 8) & 0xFF);
		out[offset++] = (uint8_t)(list_len & 0xFF);
	}

	/* NameType (0 = host_name, the only defined type) */
	out[offset++] = 0;

	/* Name length (2 bytes, big-endian) */
	{
		uint16_t name_len = (uint16_t)hostname_len;
		out[offset++] = (uint8_t)((name_len >> 8) & 0xFF);
		out[offset++] = (uint8_t)(name_len & 0xFF);
	}

	/* Hostname itself */
	memcpy(out + offset, hostname, (size_t)hostname_len);
	offset += hostname_len;

	return offset;
}

int tls_parse_sni_ext(const uint8_t *ext_body, int ext_body_len,
                      struct tls_conn *conn)
{
	int offset = 0;
	int list_len;
	int name_type;
	int name_len;

	if (!ext_body || !conn)
		return -EINVAL;
	if (ext_body_len < 2)
		return 0;  /* not an error — empty SNI list */

	/* ServerNameList length (2 bytes, big-endian) */
	list_len = ((int)ext_body[0] << 8) | (int)ext_body[1];
	offset += 2;

	if (list_len < 3 || list_len > ext_body_len - 2)
		return -EINVAL;

	/* ServerName entry: NameType(1) + length(2) + name */
	if (offset + 3 > ext_body_len)
		return -EINVAL;

	name_type = ext_body[offset++];  /* 0 = host_name */

	/* Name length (2 bytes, big-endian) */
	name_len = ((int)ext_body[offset] << 8) | (int)ext_body[offset + 1];
	offset += 2;

	/* Validate the name */
	if (name_len < 1 || name_len > TLS_SNI_MAX_HOSTNAME_LEN)
		return -EINVAL;
	if (offset + name_len > ext_body_len)
		return -EINVAL;

	/* Only host_name (type 0) is supported */
	if (name_type == 0) {
		conn->sni_hostname_len = (uint8_t)name_len;
		memcpy(conn->sni_hostname, ext_body + offset,
		       (size_t)name_len);
		conn->sni_negotiated = 1;
	}

	return 0;
}

int tls_sni_set_hostname(struct tls_conn *conn,
                         const char *hostname, int len)
{
	if (!conn)
		return -EINVAL;
	if (len < 0 || len > TLS_SNI_MAX_HOSTNAME_LEN)
		return -EINVAL;
	if (len > 0 && !hostname)
		return -EINVAL;

	if (len > 0) {
		memcpy(conn->sni_hostname, hostname, (size_t)len);
		conn->sni_hostname_len = (uint8_t)len;
	} else {
		conn->sni_hostname_len = 0;
	}
	conn->sni_negotiated = 0;

	return 0;
}

const char *tls_sni_get_hostname(const struct tls_conn *conn)
{
	if (!conn)
		return NULL;
	if (!conn->sni_negotiated || conn->sni_hostname_len == 0)
		return NULL;

	return (const char *)conn->sni_hostname;
}

/* ══════════════════════════════════════════════════════════════════════════
 * TLS 1.3 0-RTT Early Data (RFC 8446 §2.3, §4.2.10)
 *
 * 0-RTT allows a client to send "early data" using a previously
 * established PSK (from a session ticket) immediately after the
 * ClientHello, without waiting for the server's reply.
 *
 * Key schedule:
 *   early_secret = HKDF-Extract(0, PSK)
 *   client_early_traffic_secret
 *                = Derive-Secret(early_secret, "c e traffic",
 *                                ClientHello)
 *   early_exporter_master_secret
 *                = Derive-Secret(early_secret, "e exp master",
 *                                ClientHello)
 *
 * The EndOfEarlyData handshake message (type 5, empty body) signals
 * the end of 0-RTT data and the switch to regular handshake keys.
 * ══════════════════════════════════════════════════════════════════════════ */

int tls_derive_early_traffic_keys(struct tls_conn *conn,
                                  const struct tls_psk *psk,
                                  const uint8_t *client_hello,
                                  int client_hello_len)
{
	uint8_t early_secret[32];
	uint8_t client_hello_hash[32];
	uint8_t zero_salt[32];

	if (!conn || !client_hello || client_hello_len <= 0)
		return -EINVAL;

	/* 1. Hash the ClientHello handshake frame for the transcript */
	sha256_hash(client_hello_hash, client_hello, (size_t)client_hello_len);
	memcpy(conn->transcript_hash, client_hello_hash, SHA256_DIGEST_SIZE);
	conn->transcript_hash_len = SHA256_DIGEST_SIZE;

	/* 2. Derive early_secret
	 *
	 *    RFC 8446 §7.1:
	 *    early_secret = HKDF-Extract(salt = 0, ikm = PSK)
	 *
	 *    If no PSK is available (the common non-0-RTT case), derive
	 *    using a zero IKM so the early_secret is still defined,
	 *    but 0-RTT will not be enabled.
	 */
	memset(zero_salt, 0, sizeof(zero_salt));

	if (psk && psk->valid && psk->identity_len > 0) {
		tls_hkdf_extract(zero_salt, sizeof(zero_salt),
		                 psk->secret, sizeof(psk->secret),
		                 early_secret);

		memcpy(conn->psk.secret, psk->secret, sizeof(conn->psk.secret));
		conn->psk.identity_len = psk->identity_len;
		if (psk->identity_len > 0)
			memcpy(conn->psk.identity, psk->identity,
			       (size_t)psk->identity_len);
		conn->psk.max_early_data = psk->max_early_data;
		conn->psk.lifetime       = psk->lifetime;
		conn->psk.ticket_age_add = psk->ticket_age_add;
		conn->psk.valid          = 1;
		conn->has_psk            = 1;
	} else {
		uint8_t zero_ikm[32];

		memset(zero_ikm, 0, sizeof(zero_ikm));
		tls_hkdf_extract(zero_salt, sizeof(zero_salt),
		                 zero_ikm, sizeof(zero_ikm),
		                 early_secret);
		conn->has_psk = 0;
	}

	/* 3. Derive client_early_traffic_secret
	 *    This becomes the write key for early data.
	 */
	tls_derive_secret(early_secret, "c e traffic",
	                  client_hello_hash,
	                  conn->early_wstate.enc_key);

	/* 4. Derive early_exporter_master_secret
	 *    (stored in early_rstate as placeholder for server-side use)
	 */
	tls_derive_secret(early_secret, "e exp master",
	                  client_hello_hash,
	                  conn->early_rstate.enc_key);

	/* 5. Configure early data cipher states */
	conn->early_wstate.is_active    = 1;
	conn->early_wstate.enc_key_len  = 32;
	conn->early_wstate.seq_num      = 0;
	conn->early_wstate.cipher_suite = conn->wstate.cipher_suite;

	conn->early_rstate.is_active    = 1;
	conn->early_rstate.enc_key_len  = 32;
	conn->early_rstate.seq_num      = 0;
	conn->early_rstate.cipher_suite = conn->rstate.cipher_suite;

	/* 6. Enable early data */
	conn->early_data_active = 1;

	if (conn->has_psk) {
		conn->max_early_data = (psk && psk->max_early_data > 0)
		                       ? psk->max_early_data
		                       : TLS_MAX_EARLY_DATA_SIZE;
	} else {
		conn->max_early_data = 0;
	}

	/* Zeroise intermediate secrets */
	memset(early_secret, 0, sizeof(early_secret));

	return 0;
}

/* ── EndOfEarlyData (RFC 8446 §4.6.1) ──────────────────────────────────
 *
 * struct { } EndOfEarlyData;
 *
 * The EndOfEarlyData handshake message has no body — just the 4-byte
 * handshake header with msg_type = TLS_HT_END_OF_EARLY_DATA (5) and
 * length = 0.
 */

int tls_build_end_of_early_data(uint8_t *out, int out_cap)
{
	return tls_hs_build_frame(TLS_HT_END_OF_EARLY_DATA, NULL, 0,
	                          out, out_cap);
}

int tls_parse_end_of_early_data(const uint8_t *in, int in_len)
{
	uint8_t msg_type;
	int     body_len;
	int     ret;

	if (!in)
		return -EINVAL;

	ret = tls_hs_parse_header(in, in_len, &msg_type, &body_len);
	if (ret < 0)
		return ret;

	if (msg_type != TLS_HT_END_OF_EARLY_DATA)
		return -EPROTO;

	if (body_len != 0)
		return -EPROTO;

	return 0;
}

/* ── Early Data Extension (RFC 8446 §4.2.10) ──────────────────────────────
 *
 * The early_data extension code point is 0x002B (43).
 *
 * In EncryptedExtensions (server → client): empty body — just signals
 * that the server will accept 0-RTT data.
 *
 * In NewSessionTicket (server → client): contains max_early_data_size:
 *   uint32 max_early_data_size;
 *
 * If max_early_data_size is 0, an empty extension is emitted (server
 * accepts early data with no explicit size limit from this particular
 * message).  If max_early_data_size > 0, it is encoded as a 4-byte
 * big-endian value.
 */

int tls_build_early_data_ext(uint32_t max_early_data_size,
                              uint8_t *out, int out_cap)
{
	int ext_data_len;

	if (!out)
		return -EINVAL;

	ext_data_len = (max_early_data_size > 0) ? 4 : 0;

	if (out_cap < 4 + ext_data_len)
		return -ENOSPC;

	/* Extension type (2 bytes, big-endian) */
	out[0] = (uint8_t)(TLS_EXT_EARLY_DATA >> 8);
	out[1] = (uint8_t)(TLS_EXT_EARLY_DATA & 0xFF);

	/* Extension data length (2 bytes, big-endian) */
	out[2] = (uint8_t)(ext_data_len >> 8);
	out[3] = (uint8_t)(ext_data_len & 0xFF);

	/* Extension payload */
	if (max_early_data_size > 0) {
		out[4] = (uint8_t)((max_early_data_size >> 24) & 0xFF);
		out[5] = (uint8_t)((max_early_data_size >> 16) & 0xFF);
		out[6] = (uint8_t)((max_early_data_size >>  8) & 0xFF);
		out[7] = (uint8_t)(max_early_data_size & 0xFF);
		return 8;
	}

	return 4;
}

/* ── 0-RTT Early Data Encrypt / Decrypt ─────────────────────────────────
 *
 * Early data uses the existing record layer API, but with the separate
 * early_data cipher state (early_wstate / early_rstate).  The content
 * type is APPLICATION_DATA (23) — identical to normal application data.
 */

int tls_early_data_encrypt(struct tls_conn *conn,
                           const uint8_t *data, int data_len,
                           uint8_t *out, int out_cap)
{
	int offset = 0;
	int total_written = 0;

	if (!conn || !data || !out)
		return -EINVAL;
	if (!conn->early_data_active || !conn->has_psk)
		return -EPERM;
	if (data_len <= 0)
		return 0;
	if (conn->max_early_data > 0 &&
	    (uint32_t)data_len > conn->max_early_data)
		return -EMSGSIZE;

	/* Fragment and encrypt using the early data write state.
	 * This mirrors tls_record_send() but uses conn->early_wstate
	 * instead of conn->wstate. */
	while (offset < data_len) {
		int chunk = data_len - offset;
		int ret;

		if (chunk > TLS_MAX_PLAINTEXT_LEN)
			chunk = TLS_MAX_PLAINTEXT_LEN;

		ret = tls_record_encrypt(&conn->early_wstate,
		                         TLS_CT_APPLICATION_DATA,
		                         conn->version,
		                         data + offset, chunk,
		                         out + total_written,
		                         out_cap - total_written);
		if (ret < 0)
			return ret;

		total_written += ret;
		offset        += chunk;
	}

	return total_written;
}

int tls_early_data_decrypt(struct tls_conn *conn,
                           const uint8_t *in, int in_len,
                           uint8_t *data, int data_cap)
{
	uint8_t content_type;
	int     ret;

	if (!conn || !in || !data)
		return -EINVAL;
	if (!conn->early_data_active)
		return -EPERM;
	if (in_len <= 0)
		return 0;

	/* Decrypt using the early data read state directly.
	 * We parse the record header manually so we can pass the
	 * early cipher state to tls_record_decrypt(). */
	if (in_len < TLS_RECORD_HEADER_LEN)
		return -EINVAL;

	{
		const struct tls_record_header *hdr;
		const uint8_t *cipher_body;
		int cipher_len;

		hdr        = (const struct tls_record_header *)in;
		cipher_len = ntohs(hdr->length);

		if (TLS_RECORD_HEADER_LEN + cipher_len > in_len)
			return -EINVAL;
		if (cipher_len > data_cap)
			return -ENOSPC;

		content_type = hdr->content_type;
		cipher_body  = in + TLS_RECORD_HEADER_LEN;

		ret = tls_record_decrypt(&conn->early_rstate, hdr,
		                         cipher_body, cipher_len,
		                         data, data_cap);
		if (ret < 0)
			return ret;
	}

	if (content_type != TLS_CT_APPLICATION_DATA)
		return -EPROTO;

	return ret;
}

/* ── Early Data State Initialisation ─────────────────────────────────── */

void tls_early_data_init(struct tls_conn *conn)
{
	if (!conn)
		return;

	memset(&conn->early_wstate, 0, sizeof(conn->early_wstate));
	memset(&conn->early_rstate, 0, sizeof(conn->early_rstate));
	memset(&conn->psk, 0, sizeof(conn->psk));
	conn->max_early_data   = 0;
	conn->has_psk          = 0;
	conn->early_data_active = 0;
	conn->early_state      = TLS_ED_NONE;
	conn->transcript_hash_len = 0;
}

/* ── Session Ticket Helper ──────────────────────────────────────────── */

/*
 * tls_send_new_session_ticket — Build a NewSessionTicket handshake frame
 * from the current connection state.
 *
 * Creates a session ticket containing the negotiated parameters (cipher
 * suite, PSK material) and wraps it in a TLS handshake frame ready to
 * send as a TLS_CT_HANDSHAKE record.
 *
 * On success returns the number of bytes written to 'out' (the complete
 * handshake frame).  On failure returns a negative errno (callers should
 * treat non-fatal failures as "skip ticket" rather than handshake abort).
 */
static int tls_send_new_session_ticket(struct tls_conn *conn,
                                        uint8_t *out, int out_cap)
{
	const struct tls_ticket_ctx *ticket_ctx;
	struct tls_session_ticket_data sess_data;
	uint8_t ticket_buf[TLS_MAX_TICKET_LEN];
	int ticket_len;
	uint32_t ticket_age_add;
	uint32_t key_id;
	uint8_t nonce_buf[TLS_TICKET_NONCE_LEN];
	uint8_t nst_body[128 + TLS_MAX_TICKET_LEN];
	int nst_body_len;
	int ret;

	if (!conn || !out)
		return -EINVAL;

	/* Generate a random nonce */
	{
		static uint64_t counter = 0;
		uint32_t seed = (uint32_t)(counter++);
		for (int i = 0; i < TLS_TICKET_NONCE_LEN; i++) {
			seed = seed * 1103515245U + 12345U;
			nonce_buf[i] = (uint8_t)(seed >> 16);
		}
	}

	/* Build session data from the current connection state */
	memset(&sess_data, 0, sizeof(sess_data));
	sess_data.protocol_version = conn->version;
	sess_data.cipher_suite = conn->wstate.cipher_suite;
	sess_data.session_id_len = conn->session_id_len;
	if (conn->session_id_len > 0)
		memcpy(sess_data.session_id, conn->session_id,
		       (size_t)conn->session_id_len);
	/* Use the handshake transcript as PSK secret filler */
	memcpy(sess_data.psk_secret, conn->transcript_hash,
	       sizeof(sess_data.psk_secret));
	sess_data.timestamp = 0;  /* placeholder */

	/* Look up the global ticket context */
	ticket_ctx = tls_get_ticket_ctx();
	if (!ticket_ctx) {
		/* Ticket subsystem not initialised — skip ticket */
		return -ENOSYS;
	}

	/* Encrypt the session data into a ticket */
	ticket_len = tls_ticket_create(ticket_ctx, &sess_data,
	                               ticket_buf, sizeof(ticket_buf),
	                               &ticket_age_add, &key_id);
	if (ticket_len < 0)
		return ticket_len;

	(void)key_id;  /* key_id embedded in ticket, not needed separately */

	/* Build the NewSessionTicket message body */
	nst_body_len = tls_build_new_session_ticket(
		ticket_buf, ticket_len,
		TLS_TICKET_LIFETIME_DEFAULT,
		ticket_age_add,
		nonce_buf, TLS_TICKET_NONCE_LEN,
		nst_body, sizeof(nst_body));
	if (nst_body_len < 0)
		return nst_body_len;

	/* Wrap in handshake frame */
	ret = tls_hs_build_frame(TLS_HT_NEW_SESSION_TICKET,
	                         nst_body, nst_body_len,
	                         out, out_cap);
	return ret;
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
EXPORT_SYMBOL(tls_hkdf_extract);
EXPORT_SYMBOL(tls_hkdf_expand);
EXPORT_SYMBOL(tls_hkdf_expand_label);
EXPORT_SYMBOL(tls_derive_secret);
EXPORT_SYMBOL(tls_derive_early_traffic_keys);
EXPORT_SYMBOL(tls_build_end_of_early_data);
EXPORT_SYMBOL(tls_parse_end_of_early_data);
EXPORT_SYMBOL(tls_build_early_data_ext);
EXPORT_SYMBOL(tls_early_data_encrypt);
EXPORT_SYMBOL(tls_early_data_decrypt);
EXPORT_SYMBOL(tls_early_data_init);
EXPORT_SYMBOL(tls_build_hello_request);
EXPORT_SYMBOL(tls_parse_hello_request);
EXPORT_SYMBOL(tls_build_renegotiation_info_ext);
EXPORT_SYMBOL(tls_parse_renegotiation_info_ext);
EXPORT_SYMBOL(tls_conn_request_renegotiate);
EXPORT_SYMBOL(tls_handle_renegotiation_client_hello);
EXPORT_SYMBOL(tls_build_alpn_ext);
EXPORT_SYMBOL(tls_parse_alpn_ext);
EXPORT_SYMBOL(tls_alpn_set_protocols);
EXPORT_SYMBOL(tls_alpn_get_selected);

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
