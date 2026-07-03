/* tls.c — TLS record layer: content types, fragmentation, encryption */

#include "tls.h"
#include "tls_aead.h"
#include "crypto.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "export.h"

/* ── Global TLS Subsystem State ────────────────────────────────────── */

static int tls_initialised;
static spinlock_t tls_lock = SPINLOCK_INIT;

/* ── Initialisation ────────────────────────────────────────────────── */

int __init tls_init(void)
{
	spinlock_acquire(&tls_lock);
	if (!tls_initialised) {
		tls_initialised = 1;
		kprintf("[OK] tls: TLS record layer initialised (supports "
		        "0x%04x–0x%04x)\n", TLS_VER_1_2, TLS_VER_1_3);
	}
	spinlock_release(&tls_lock);

	/* Initialise session ticket subsystem */
	{
		int ticket_ret = tls_session_init();
		if (ticket_ret < 0) {
			kprintf("[tls] warning: session ticket init "
			        "failed: %d\n", ticket_ret);
		}
	}

	return 0;
}

/* ── Connection Lifecycle ──────────────────────────────────────────── */

int tls_conn_init(struct tls_conn *conn, int is_client, uint16_t version)
{
	if (!conn)
		return -EINVAL;

	memset(conn, 0, sizeof(*conn));
	conn->is_client = is_client;
	conn->version   = version;
	conn->recv_len  = 0;
	tls_early_data_init(conn);
	return 0;
}

void tls_conn_cleanup(struct tls_conn *conn)
{
	if (!conn)
		return;

	/* Zap all crypto material before freeing */
	memset(conn, 0, sizeof(*conn));
}

/* ── Internal Helpers ──────────────────────────────────────────────── */

static inline void tls_write16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xFF);
}

static inline void tls_write64(uint8_t *p, uint64_t v)
{
	for (int i = 7; i >= 0; i--) {
		p[i] = (uint8_t)(v & 0xFF);
		v >>= 8;
	}
}

/* ── Record Protection (Encrypt / Decrypt) ─────────────────────────── */

/*
 * tls_record_encrypt — Encrypt a single TLS record fragment.
 *
 * Constructs the record header, forms the AEAD nonce from the cipher
 * state, computes the AAD, and encrypts using the appropriate AEAD
 * cipher (AES-GCM or ChaCha20-Poly1305).
 *
 * On success returns the total number of bytes written to 'out'
 * (header + ciphertext body + AEAD tag).  On failure returns a
 * negative errno.
 */
int tls_record_encrypt(struct tls_cipher_state *cs,
                       uint8_t content_type, uint16_t version,
                       const uint8_t *plain, int plain_len,
                       uint8_t *out, int out_cap)
{
	struct tls_record_header *hdr;
	struct tls_aead_ctx aead;
	uint8_t nonce[TLS_AEAD_NONCE_LEN];
	uint8_t aad[13];    /* TLS AAD: content_type(1) + version(2) + length(2)
	                       + (TLS 1.2: seq_num(8)) */
	uint8_t tag[TLS_AEAD_MAX_TAG_LEN];
	int aad_len;
	int total_len;
	int ret;

	if (!cs || !plain || !out)
		return -EINVAL;
	if (plain_len < 0 || plain_len > TLS_MAX_PLAINTEXT_LEN)
		return -EINVAL;

	/* Determine AEAD parameters from cipher suite */
	ret = tls_aead_init(&aead, cs->enc_key, cs->enc_key_len,
	                     cs->cipher_suite);
	if (ret < 0)
		return ret;

	total_len = TLS_RECORD_HEADER_LEN + plain_len + aead.tag_len;
	if (out_cap < total_len)
		return -ENOSPC;

	/* Build the 5‑byte record header */
	hdr = (struct tls_record_header *)out;
	hdr->content_type = content_type;
	hdr->version      = htons(version);
	hdr->length       = htons((uint16_t)(plain_len + aead.tag_len));

	/* Build per-record nonce */
	tls_aead_build_nonce(cs, version, nonce);

	/* Build AAD */
	if (version <= TLS_VER_1_2) {
		/* TLS 1.2 AAD: seq_num(8 BE) || content_type || version || length */
		uint64_t seq = cs->seq_num;
		int j;
		for (j = 7; j >= 0; j--) {
			aad[j]     = (uint8_t)(seq & 0xFF);
			seq >>= 8;
		}
		aad[8] = content_type;
		aad[9] = (uint8_t)((version >> 8) & 0xFF);
		aad[10] = (uint8_t)(version & 0xFF);
		aad[11] = (uint8_t)(((plain_len + aead.tag_len) >> 8) & 0xFF);
		aad[12] = (uint8_t)((plain_len + aead.tag_len) & 0xFF);
		aad_len = 13;
	} else {
		/* TLS 1.3 AAD: content_type || version || length */
		aad[0] = content_type;
		aad[1] = (uint8_t)((version >> 8) & 0xFF);
		aad[2] = (uint8_t)(version & 0xFF);
		aad[3] = (uint8_t)(((plain_len + aead.tag_len) >> 8) & 0xFF);
		aad[4] = (uint8_t)((plain_len + aead.tag_len) & 0xFF);
		aad_len = 5;
	}

	/* Encrypt */
	ret = tls_aead_encrypt(&aead, nonce,
	                        aad, aad_len,
	                        plain, plain_len,
	                        out + TLS_RECORD_HEADER_LEN, tag);
	if (ret < 0)
		return ret;

	/* Append tag after ciphertext */
	memcpy(out + TLS_RECORD_HEADER_LEN + plain_len, tag, (size_t)aead.tag_len);

	/* Advance sequence number for anti‑replay / nonce construction */
	cs->seq_num++;

	return total_len;
}

/*
 * tls_record_decrypt — Decrypt a single TLS record.
 *
 * Strips the 5-byte header, validates the AEAD authentication tag,
 * and decrypts the payload using the appropriate AEAD cipher.
 *
 * On success returns the payload length (bytes written to 'data').
 * On failure returns a negative errno (including -EBADMSG on auth failure).
 */
int tls_record_decrypt(struct tls_cipher_state *cs,
                       const struct tls_record_header *hdr,
                       const uint8_t *cipher, int cipher_len,
                       uint8_t *data, int data_cap)
{
	struct tls_aead_ctx aead;
	uint8_t nonce[TLS_AEAD_NONCE_LEN];
	uint8_t aad[13];
	int aad_len;
	int payload_len;
	int tag_len;
	int ret;

	if (!hdr || !cipher || !data)
		return -EINVAL;

	payload_len = ntohs(hdr->length);
	if (payload_len < 0 || payload_len > TLS_MAX_CIPHERTEXT_LEN)
		return -EINVAL;
	if (payload_len > data_cap + 16)  /* allow for tag */
		return -ENOSPC;

	/* Determine AEAD parameters from cipher suite */
	ret = tls_aead_init(&aead, cs->enc_key, cs->enc_key_len,
	                     cs->cipher_suite);
	if (ret < 0)
		return ret;

	tag_len = aead.tag_len;

	if (payload_len < tag_len)
		return -EINVAL;

	/* Split ciphertext into encrypted body and trailing tag */
	int enc_len = payload_len - tag_len;
	const uint8_t *enc_body = cipher;
	const uint8_t *tag = cipher + enc_len;

	if (enc_len > data_cap)
		return -ENOSPC;

	/* Build per-record nonce */
	tls_aead_build_nonce(cs, ntohs(hdr->version), nonce);

	/* Build AAD */
	if (ntohs(hdr->version) <= TLS_VER_1_2) {
		uint64_t seq = cs->seq_num;
		int j;
		for (j = 7; j >= 0; j--) {
			aad[j]     = (uint8_t)(seq & 0xFF);
			seq >>= 8;
		}
		aad[8] = hdr->content_type;
		aad[9] = (uint8_t)((ntohs(hdr->version) >> 8) & 0xFF);
		aad[10] = (uint8_t)(ntohs(hdr->version) & 0xFF);
		aad[11] = (uint8_t)((payload_len >> 8) & 0xFF);
		aad[12] = (uint8_t)(payload_len & 0xFF);
		aad_len = 13;
	} else {
		aad[0] = hdr->content_type;
		aad[1] = (uint8_t)((ntohs(hdr->version) >> 8) & 0xFF);
		aad[2] = (uint8_t)(ntohs(hdr->version) & 0xFF);
		aad[3] = (uint8_t)((payload_len >> 8) & 0xFF);
		aad[4] = (uint8_t)(payload_len & 0xFF);
		aad_len = 5;
	}

	/* Decrypt with AEAD (verifies tag) */
	ret = tls_aead_decrypt(&aead, nonce,
	                        aad, aad_len,
	                        enc_body, enc_len,
	                        tag, data);
	if (ret < 0)
		return ret;

	return enc_len;
}

/* ── Record Layer Send / Receive ───────────────────────────────────── */

/*
 * tls_record_send — Build one or more TLS records from plaintext.
 *
 * If data_len exceeds TLS_MAX_PLAINTEXT_LEN (16384 B) the data is
 * fragmented into multiple independent records, each encrypted with
 * an independent sequence number.
 *
 * Returns the total number of bytes written to 'out', or < 0 on error.
 */
int tls_record_send(struct tls_conn *conn, uint8_t content_type,
                    const uint8_t *data, int data_len,
                    uint8_t *out, int out_cap)
{
	int offset     = 0;
	int total_written = 0;

	if (!conn || !data || !out)
		return -EINVAL;
	if (data_len < 0)
		return -EINVAL;
	if (data_len == 0)
		return 0;

	while (offset < data_len) {
		int chunk = data_len - offset;
		int ret;

		if (chunk > TLS_MAX_PLAINTEXT_LEN)
			chunk = TLS_MAX_PLAINTEXT_LEN;

		ret = tls_record_encrypt(&conn->wstate, content_type,
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

/*
 * tls_record_recv — Parse and decrypt a single TLS record.
 *
 * Expects 'in' to contain at least TLS_RECORD_HEADER_LEN bytes.
 * On success sets *content_type and writes the decrypted payload
 * into 'data'.  Returns the payload length, or < 0 on error.
 */
int tls_record_recv(struct tls_conn *conn,
                    const uint8_t *in, int in_len,
                    uint8_t *content_type,
                    uint8_t *data, int data_cap)
{
	const struct tls_record_header *hdr;
	const uint8_t *cipher_body;
	int cipher_len;
	int ret;

	if (!conn || !in || !content_type || !data)
		return -EINVAL;
	if (in_len < TLS_RECORD_HEADER_LEN)
		return -EINVAL;

	hdr        = (const struct tls_record_header *)in;
	cipher_len = ntohs(hdr->length);

	if (TLS_RECORD_HEADER_LEN + cipher_len > in_len)
		return -EINVAL;
	if (cipher_len > data_cap)
		return -ENOSPC;

	cipher_body = in + TLS_RECORD_HEADER_LEN;

	*content_type = hdr->content_type;

	ret = tls_record_decrypt(&conn->rstate, hdr,
	                         cipher_body, cipher_len,
	                         data, data_cap);
	if (ret < 0)
		return ret;

	return ret;
}
