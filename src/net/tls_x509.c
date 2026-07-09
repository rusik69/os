/* tls_x509.c — X.509 certificate parsing (DER skeleton)
 *
 * Implements minimal DER-based X.509 certificate parsing sufficient
 * to extract certificate fields, compute fingerprints, and build/parse
 * the TLS Certificate handshake message (RFC 8446 §4.4.2).
 *
 * References:
 *   RFC 5280 — Internet X.509 Public Key Infrastructure
 *   RFC 8446 §4.4.2 — TLS 1.3 Certificate message
 *   ITU-T X.690 — ASN.1 DER encoding
 */

#include "tls_x509.h"
#include "tls.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "export.h"
#include "sha256.h"

/* ══════════════════════════════════════════════════════════════════════
 * DER Parsing Helpers
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * x509_parse_der_length — Parse a DER length field.
 *
 * Short form (1 byte):   0xxxxxxx  — length is the lower 7 bits (0..127)
 * Long form (2..127):    1xxxxxxx  — lower 7 bits = number of subsequent
 *                                     bytes encoding the length (big-endian)
 */
int x509_parse_der_length(const uint8_t *der, uint32_t *len)
{
	uint8_t first;
	int num_bytes;

	if (!der || !len)
		return -EINVAL;

	first = der[0];
	if (first < 0x80) {
		/* Short form */
		*len = first;
		return 1;
	}

	/* Long form */
	num_bytes = first & 0x7F;
	if (num_bytes == 0 || num_bytes > 4) {
		/* Indefinite length or too long for our parser */
		return -EINVAL;
	}

	*len = 0;
	for (int i = 0; i < num_bytes; i++) {
		*len = (*len << 8) | (uint32_t)der[1 + i];
	}

	return 1 + num_bytes;
}
EXPORT_SYMBOL(x509_parse_der_length);

/*
 * x509_skip_der_tlv — Skip a DER TLV (tag + length + value).
 *
 * Returns the total size of the TLV, or a negative errno.
 */
int x509_skip_der_tlv(const uint8_t *der, int size)
{
	uint32_t vlen;
	int len_bytes;

	if (!der || size < 2)
		return -EINVAL;

	len_bytes = x509_parse_der_length(der + 1, &vlen);
	if (len_bytes < 0)
		return len_bytes;

	/* tag(1) + length(len_bytes) + value(vlen) */
	return 1 + len_bytes + (int)vlen;
}
EXPORT_SYMBOL(x509_skip_der_tlv);

/*
 * x509_read_der_tlv — Parse a DER TLV into its components.
 *
 * Returns the total TLV size (tag + length + value), or a negative errno.
 */
int x509_read_der_tlv(const uint8_t *der, int size,
                       uint8_t *tag, const uint8_t **value, uint32_t *vlen)
{
	uint32_t len_val;
	int len_bytes;
	int total;

	if (!der || !tag || !value || !vlen)
		return -EINVAL;
	if (size < 2)
		return -EINVAL;

	*tag = der[0];
	len_bytes = x509_parse_der_length(der + 1, &len_val);
	if (len_bytes < 0)
		return len_bytes;

	total = 1 + len_bytes + (int)len_val;
	if (total > size)
		return -EINVAL;

	*value = der + 1 + len_bytes;
	*vlen  = len_val;
	return total;
}
EXPORT_SYMBOL(x509_read_der_tlv);

/*
 * x509_match_oid — Check if a DER OID at the given offset matches.
 *
 * Returns 1 if matched, 0 if not, or a negative errno on invalid DER.
 */
int x509_match_oid(const uint8_t *der, int offset, int size,
                    const uint8_t *oid, int oid_len)
{
	uint32_t len_val;
	int len_bytes;

	if (!der || !oid || offset < 0 || size < offset + 2)
		return -EINVAL;

	if (der[offset] != DER_TAG_OID)
		return 0;

	len_bytes = x509_parse_der_length(der + offset + 1, &len_val);
	if (len_bytes < 0)
		return len_bytes;

	if ((int)len_val != (oid_len - 2)) /* oid passed includes tag+length */
		return 0;

	if (offset + 1 + len_bytes + (int)len_val > size)
		return -EINVAL;

	/* Compare value bytes only (skip the tag and length) */
	return (memcmp(der + offset + 1 + len_bytes,
	               oid + 2, (size_t)(oid_len - 2)) == 0) ? 1 : 0;
}
EXPORT_SYMBOL(x509_match_oid);

/* ══════════════════════════════════════════════════════════════════════
 * Internal DER Navigation Helpers
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * x509_enter_sequence — Expect a SEQUENCE tag, return value pointer + len.
 */
static int x509_enter_sequence(const uint8_t *der, int size,
                                const uint8_t **value, uint32_t *vlen)
{
	uint8_t tag;
	int ret;

	ret = x509_read_der_tlv(der, size, &tag, value, vlen);
	if (ret < 0)
		return ret;
	if (tag != DER_TAG_SEQUENCE)
		return -EINVAL;
	return ret;
}

/*
 * x509_read_integer — Read a DER INTEGER, copy value into a buffer.
 */
static int x509_read_integer(const uint8_t *der, int size,
                              uint8_t *buf, int buf_len, int *out_len)
{
	uint8_t tag;
	const uint8_t *value;
	uint32_t vlen;
	int ret;

	ret = x509_read_der_tlv(der, size, &tag, &value, &vlen);
	if (ret < 0)
		return ret;
	if (tag != DER_TAG_INTEGER)
		return -EINVAL;
	if (vlen == 0)
		return -EINVAL;
	if ((int)vlen > buf_len)
		return -ENOSPC;

	/* Strip leading zero if present (positive INTEGER encoding) */
	if (value[0] == 0 && vlen > 1) {
		value++;
		vlen--;
	}

	memcpy(buf, value, (size_t)vlen);
	*out_len = (int)vlen;
	return ret;
}

/*
 * x509_read_algo_id — Read an AlgorithmIdentifier (SEQUENCE { OID, params }).
 */
static int x509_read_algo_id(const uint8_t *der, int size,
                              struct tls_x509_algo_id *algo)
{
	const uint8_t *seq_val;
	uint32_t seq_len;
	uint8_t oid_tag;
	const uint8_t *oid_val;
	uint32_t oid_vlen;
	int offset;
	int ret;

	memset(algo, 0, sizeof(*algo));

	ret = x509_enter_sequence(der, size, &seq_val, &seq_len);
	if (ret < 0)
		return ret;

	/* Read OID */
	ret = x509_read_der_tlv(seq_val, (int)seq_len,
	                         &oid_tag, &oid_val, &oid_vlen);
	if (ret < 0)
		return ret;
	if (oid_tag != DER_TAG_OID)
		return -EINVAL;
	if ((int)oid_vlen > (int)sizeof(algo->oid))
		return -ENOSPC;

	algo->oid[0] = DER_TAG_OID;
	{
		int lret = x509_parse_der_length(seq_val + 1, &oid_vlen);
		if (lret < 0)
			return lret;
		/* Use the inner TLV bytes for the OID */
		const uint8_t *oid_start = seq_val;
		int            oid_total = ret;  /* tlv size */
		if (oid_total > (int)sizeof(algo->oid))
			oid_total = (int)sizeof(algo->oid);
		memcpy(algo->oid, oid_start, (size_t)oid_total);
		algo->oid_len = oid_total;
	}

	offset = ret;

	/* Optional parameters — just store raw bytes if present */
	if (offset < (int)seq_len) {
		uint8_t param_tag;
		const uint8_t *param_val;
		uint32_t param_vlen;
		int pret;

		pret = x509_read_der_tlv(seq_val + offset,
		                          (int)(seq_len - (uint32_t)offset),
		                          &param_tag, &param_val, &param_vlen);
		if (pret > 0) {
			int param_total = pret;
			if (param_total > (int)sizeof(algo->params))
				param_total = (int)sizeof(algo->params);
			memcpy(algo->params, seq_val + offset, (size_t)param_total);
			algo->params_len = param_total;
		}
	}

	return ret;
}

/* ══════════════════════════════════════════════════════════════════════
 * X.509 Certificate Parsing
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * x509_parse_cert — Parse a single DER-encoded X.509 certificate.
 *
 * Certificate DER structure (RFC 5280 §4.1):
 *   Certificate ::= SEQUENCE {
 *       tbsCertificate       TBSCertificate,
 *       signatureAlgorithm   AlgorithmIdentifier,
 *       signatureValue       BIT STRING
 *   }
 *
 *   TBSCertificate ::= SEQUENCE {
 *       version         [0]  EXPLICIT INTEGER DEFAULT v1,
 *       serialNumber         INTEGER,
 *       signature            AlgorithmIdentifier,
 *       issuer               Name,
 *       validity             Validity,
 *       subject              Name,
 *       subjectPublicKeyInfo SubjectPublicKeyInfo,
 *       issuerUniqueID  [1]  IMPLICIT BIT STRING OPTIONAL, -- v2/v3
 *       subjectUniqueID [2]  IMPLICIT BIT STRING OPTIONAL, -- v2/v3
 *       extensions      [3]  EXPLICIT Extensions OPTIONAL  -- v3
 *   }
 */
int x509_parse_cert(const uint8_t *der, int der_len,
                     struct tls_x509_cert *cert)
{
	const uint8_t *cert_seq;
	const uint8_t *tbs_seq;
	const uint8_t *tbs_content;
	const uint8_t *inner;
	uint32_t cert_seq_len;
	uint32_t tbs_seq_len;
	uint32_t inner_len;
	uint8_t tag;
	int offset;
	int ret;

	if (!der || !cert || der_len < 20)
		return -EINVAL;

	memset(cert, 0, sizeof(*cert));
	cert->version = 1;  /* default v1 */

	/* Parse outer Certificate SEQUENCE */
	ret = x509_enter_sequence(der, der_len, &cert_seq, &cert_seq_len);
	if (ret < 0)
		return ret;

	/* The TBS certificate is the first element inside the outer sequence */
	tbs_seq = cert_seq;
	tbs_seq_len = cert_seq_len;

	/* Save pointer to the start of TBS DER for signature verification */
	cert->tbs_der = tbs_seq;

	/* ── Parse TBSCertificate ────────────────────────────────── */
	ret = x509_enter_sequence(tbs_seq, (int)tbs_seq_len,
	                           &tbs_content, &inner_len);
	if (ret < 0)
		return ret;
	cert->tbs_len = ret;  /* length of the TBSCertificate SEQUENCE */
	offset = 0;          /* offset within tbs_content (the value) */

	/* ── Version [0] EXPLICIT INTEGER (v3 only) ──────────────── */
	if ((int)inner_len > 0 && tbs_content[0] == DER_TAG_CTX_0) {
		const uint8_t *ctx_val;
		uint32_t ctx_len;
		uint8_t ver_tag;
		const uint8_t *ver_val;
		uint32_t ver_vlen;

		ret = x509_read_der_tlv(tbs_content, (int)inner_len,
		                         &tag, &ctx_val, &ctx_len);
		if (ret < 0)
			return ret;
		if (tag != DER_TAG_CTX_0)
			return -EINVAL;

		ret = x509_read_der_tlv(ctx_val, (int)ctx_len,
		                         &ver_tag, &ver_val, &ver_vlen);
		if (ret < 0)
			return ret;
		if (ver_tag != DER_TAG_INTEGER)
			return -EINVAL;
		if (ver_vlen > 0)
			cert->version = (int)ver_val[0] + 1;

		/* Advance offset past the [0] wrapper */
		{
			uint8_t dummy_tag;
			const uint8_t *dummy_val;
			uint32_t dummy_vlen;
			int vret = x509_read_der_tlv(tbs_content, (int)inner_len,
			                              &dummy_tag, &dummy_val,
			                              &dummy_vlen);
			if (vret < 0)
				return vret;
			offset = vret;
		}
	} else {
		/* No version tag → v1 default; offset starts at 0 */
		offset = 0;
	}

	/* ── Serial Number ────────────────────────────────────────── */
	if (offset >= (int)inner_len)
		return -EINVAL;
	inner = tbs_content + offset;
	ret = x509_read_integer(inner, (int)(inner_len - (uint32_t)offset),
	                         cert->serial, X509_SERIAL_MAX_LEN,
	                         &cert->serial_len);
	if (ret < 0)
		return ret;
	offset += ret;

	/* ── Signature Algorithm ──────────────────────────────────── */
	if (offset >= (int)inner_len)
		return -EINVAL;
	inner = tbs_content + offset;
	ret = x509_read_algo_id(inner, (int)(inner_len - (uint32_t)offset),
	                         &cert->signature_algo);
	if (ret < 0)
		return ret;
	offset += ret;

	/* ── Issuer (Name / SEQUENCE of SET of AttributeTypeAndValue) ── */
	if (offset >= (int)inner_len)
		return -EINVAL;
	inner = tbs_content + offset;
	{
		const uint8_t *issuer_val;
		uint32_t issuer_vlen;
		ret = x509_read_der_tlv(inner, (int)(inner_len - (uint32_t)offset),
		                         &tag, &issuer_val, &issuer_vlen);
		if (ret < 0)
			return ret;
		/* Store the raw DN bytes */
		{
			int to_copy = (int)(issuer_vlen);
			if (to_copy > X509_DN_MAX_LEN)
				to_copy = X509_DN_MAX_LEN;
			memcpy(cert->issuer.raw, issuer_val, (size_t)to_copy);
			cert->issuer.raw_len = to_copy;
		}
		offset += ret;
	}

	/* ── Validity (SEQUENCE of notBefore, notAfter) ───────────── */
	if (offset >= (int)inner_len)
		return -EINVAL;
	inner = tbs_content + offset;
	{
		const uint8_t *valid_seq_val;
		uint32_t valid_seq_len;
		int voff;

		ret = x509_enter_sequence(inner,
		                           (int)(inner_len - (uint32_t)offset),
		                           &valid_seq_val, &valid_seq_len);
		if (ret < 0)
			return ret;
		voff = ret;

		/* notBefore */
		if ((int)valid_seq_len < 2)
			return -EINVAL;
		{
			uint8_t nbtag;
			const uint8_t *nbval;
			uint32_t nbvlen;
			ret = x509_read_der_tlv(valid_seq_val, (int)valid_seq_len,
			                         &nbtag, &nbval, &nbvlen);
			if (ret < 0)
				return ret;
			if (nbtag != DER_TAG_UTC_TIME &&
			    nbtag != DER_TAG_GENERALIZED_TIME)
				return -EINVAL;
			if ((int)nbvlen > (int)sizeof(cert->validity.not_before))
				nbvlen = sizeof(cert->validity.not_before);
			memcpy(cert->validity.not_before, nbval, (size_t)nbvlen);
			cert->validity.not_before_len = (int)nbvlen;
		}

		/* notAfter */
		voff = ret;
		if (voff >= (int)valid_seq_len)
			return -EINVAL;
		{
			uint8_t natag;
			const uint8_t *naval;
			uint32_t navlen;
			ret = x509_read_der_tlv(
				valid_seq_val + voff,
				(int)(valid_seq_len - (uint32_t)voff),
				&natag, &naval, &navlen);
			if (ret < 0)
				return ret;
			if (natag != DER_TAG_UTC_TIME &&
			    natag != DER_TAG_GENERALIZED_TIME)
				return -EINVAL;
			if ((int)navlen > (int)sizeof(cert->validity.not_after))
				navlen = sizeof(cert->validity.not_after);
			memcpy(cert->validity.not_after, naval, (size_t)navlen);
			cert->validity.not_after_len = (int)navlen;
		}

		/* Advance main offset past the validity sequence */
		{
			const uint8_t *dummy_vs;
			uint32_t dummy_vl;
			int vret = x509_enter_sequence(
				tbs_content + offset,
				(int)(inner_len - (uint32_t)offset),
				&dummy_vs, &dummy_vl);
			if (vret < 0)
				return vret;
			offset += vret;
		}
	}

	/* ── Subject (Name) ───────────────────────────────────────── */
	if (offset >= (int)inner_len)
		return -EINVAL;
	inner = tbs_content + offset;
	{
		const uint8_t *subject_val;
		uint32_t subject_vlen;
		ret = x509_read_der_tlv(inner, (int)(inner_len - (uint32_t)offset),
		                         &tag, &subject_val, &subject_vlen);
		if (ret < 0)
			return ret;
		{
			int to_copy = (int)(subject_vlen);
			if (to_copy > X509_DN_MAX_LEN)
				to_copy = X509_DN_MAX_LEN;
			memcpy(cert->subject.raw, subject_val, (size_t)to_copy);
			cert->subject.raw_len = to_copy;
		}
		offset += ret;
	}

	/* ── SubjectPublicKeyInfo ─────────────────────────────────── */
	if (offset >= (int)inner_len)
		return -EINVAL;
	inner = tbs_content + offset;
	{
		const uint8_t *spki_seq;
		uint32_t spki_seq_len;

		ret = x509_enter_sequence(inner,
		                           (int)(inner_len - (uint32_t)offset),
		                           &spki_seq, &spki_seq_len);
		if (ret < 0)
			return ret;

		/* AlgorithmIdentifier within SPKI */
		{
			int aoff = x509_read_algo_id(
				spki_seq, (int)spki_seq_len,
				&cert->pubkey.algorithm);
			if (aoff < 0)
				return aoff;

			/* BIT STRING for the public key data */
			if (aoff >= (int)spki_seq_len)
				return -EINVAL;
			{
				uint8_t bstag;
				const uint8_t *bsval;
				uint32_t bsvlen;
				ret = x509_read_der_tlv(
					spki_seq + aoff,
					(int)(spki_seq_len - (uint32_t)aoff),
					&bstag, &bsval, &bsvlen);
				if (ret < 0)
					return ret;
				if (bstag != DER_TAG_BIT_STRING)
					return -EINVAL;

				/* Skip unused-bits byte */
				if (bsvlen < 2)
					return -EINVAL;
				bsval++;
				bsvlen--;
				if ((int)bsvlen > X509_PUBKEY_MAX_LEN)
					bsvlen = X509_PUBKEY_MAX_LEN;
				memcpy(cert->pubkey.key_data, bsval, (size_t)bsvlen);
				cert->pubkey.key_data_len = (int)bsvlen;
			}
		}
	}
	offset += ret;

	/* ── Skip optional unique identifiers and extensions ─────── */
	/* (Not parsed for the skeleton) */
	(void)offset;
	(void)inner_len;

	/* ── Signature Algorithm (outer) ──────────────────────────── */
	{
		const uint8_t *remaining;
		int remain_len;

		remaining = cert_seq + cert->tbs_len;
		remain_len = (int)cert_seq_len - cert->tbs_len;

		if (remain_len < 2)
			return -EINVAL;

		ret = x509_read_algo_id(remaining, remain_len,
		                         &cert->sig_algo);
		if (ret < 0)
			return ret;

		/* ── Signature Value (BIT STRING) ─────────────────────── */
		remaining += ret;
		remain_len -= ret;
		if (remain_len < 2)
			return -EINVAL;

		{
			uint8_t sig_tag;
			const uint8_t *sig_val;
			uint32_t sig_vlen;
			ret = x509_read_der_tlv(remaining, remain_len,
			                         &sig_tag, &sig_val, &sig_vlen);
			if (ret < 0)
				return ret;
			if (sig_tag != DER_TAG_BIT_STRING)
				return -EINVAL;

			/* Skip unused-bits byte */
			if (sig_vlen < 2)
				return -EINVAL;
			sig_val++;
			sig_vlen--;
			if ((int)sig_vlen > X509_SIG_VALUE_MAX_LEN)
				sig_vlen = X509_SIG_VALUE_MAX_LEN;
			memcpy(cert->sig_value, sig_val, (size_t)sig_vlen);
			cert->sig_value_len = (int)sig_vlen;
		}
	}

	/* ── Compute SHA-256 fingerprint ──────────────────────────── */
	sha256_hash(cert->fingerprint, der, (size_t)der_len);

	return 0;
}
EXPORT_SYMBOL(x509_parse_cert);

/*
 * x509_print_cert — Log a human-readable summary of a parsed cert.
 */
void x509_print_cert(const struct tls_x509_cert *cert, const char *prefix)
{
	if (!cert || !prefix)
		return;

	if (!prefix)
		prefix = "cert";

	kprintf("[tls] %s: X.509v%d certificate, serial ",
	        prefix, cert->version);
	for (int i = 0; i < cert->serial_len; i++)
		kprintf("%02x", cert->serial[i]);
	kprintf("\n");

	kprintf("[tls] %s: subject DN length %d, issuer DN length %d\n",
	        prefix, cert->subject.raw_len, cert->issuer.raw_len);
	kprintf("[tls] %s: notBefore %d bytes, notAfter %d bytes\n",
	        prefix,
	        cert->validity.not_before_len,
	        cert->validity.not_after_len);
	kprintf("[tls] %s: public key %d bytes (algo OID len %d)\n",
	        prefix,
	        cert->pubkey.key_data_len,
	        cert->pubkey.algorithm.oid_len);
	kprintf("[tls] %s: signature len %d, SHA-256 fingerprint ",
	        prefix, cert->sig_value_len);
	for (int i = 0; i < SHA256_DIGEST_SIZE; i++)
		kprintf("%02x", cert->fingerprint[i]);
	kprintf("\n");
}
EXPORT_SYMBOL(x509_print_cert);

/* ══════════════════════════════════════════════════════════════════════
 * TLS Certificate Handshake Message (RFC 8446 §4.4.2)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * tls_build_certificate_msg — Build a TLS Certificate message body.
 *
 * Wire format:
 *   struct {
 *       ASN1Cert certificate_list<0..2^24-1>;
 *   } Certificate;
 *
 *   ASN1Cert ::= {
 *       opaque cert_data<1..2^24-1>;
 *       Extension extensions<0..2^16-1>;
 *   }
 */
int tls_build_certificate_msg(const struct tls_cert_entry *certs,
                               int num_certs,
                               uint8_t *out, int out_cap)
{
	int offset = 0;
	int list_bytes = 0;
	int i;

	if (!certs || !out)
		return -EINVAL;
	if (num_certs < 0 || num_certs > TLS_MAX_CERT_CHAIN_DEPTH)
		return -EINVAL;
	if (num_certs == 0)
		return -EINVAL;
	if (out_cap < 3)
		return -ENOSPC;

	/* Placeholder for certificate_list length (3 bytes, big-endian) */
	out[0] = 0;
	out[1] = 0;
	out[2] = 0;
	offset = 3;

	/* Write each certificate */
	for (i = 0; i < num_certs; i++) {
		uint8_t *cert_list;
		int entry_start;
		int entry_total;

		if (!certs[i].data || certs[i].data_len <= 0)
			return -EINVAL;
		if (certs[i].data_len > (1 << 24) - 2)  /* max 24-bit - overhead */
			return -EINVAL;

		entry_start = offset;

		/* Certificate data length (3 bytes, big-endian) */
		if (offset + 3 > out_cap)
			return -ENOSPC;
		out[offset + 0] = (uint8_t)((certs[i].data_len >> 16) & 0xFF);
		out[offset + 1] = (uint8_t)((certs[i].data_len >>  8) & 0xFF);
		out[offset + 2] = (uint8_t)((certs[i].data_len >>  0) & 0xFF);
		offset += 3;

		/* Certificate data */
		if (offset + certs[i].data_len > out_cap)
			return -ENOSPC;
		memcpy(out + offset, certs[i].data, (size_t)certs[i].data_len);
		offset += certs[i].data_len;

		/* Extensions length (2 bytes, big-endian) */
		if (certs[i].ext_len > 0 && certs[i].extensions) {
			if (offset + 2 + certs[i].ext_len > out_cap)
				return -ENOSPC;
			out[offset + 0] =
				(uint8_t)((certs[i].ext_len >> 8) & 0xFF);
			out[offset + 1] =
				(uint8_t)((certs[i].ext_len >> 0) & 0xFF);
			offset += 2;
			memcpy(out + offset, certs[i].extensions,
			       (size_t)certs[i].ext_len);
			offset += certs[i].ext_len;
		} else {
			/* No extensions: zero-length extension block */
			if (offset + 2 > out_cap)
				return -ENOSPC;
			out[offset + 0] = 0;
			out[offset + 1] = 0;
			offset += 2;
		}

		entry_total = offset - entry_start;
		list_bytes += entry_total;
	}

	/* Fill in the certificate_list length (3-byte prefix) */
	out[0] = (uint8_t)((list_bytes >> 16) & 0xFF);
	out[1] = (uint8_t)((list_bytes >>  8) & 0xFF);
	out[2] = (uint8_t)((list_bytes >>  0) & 0xFF);

	return offset;
}
EXPORT_SYMBOL(tls_build_certificate_msg);

/*
 * tls_parse_certificate_msg — Parse a TLS Certificate message body.
 *
 * Parses the certificate_list and fills @certs with pointers into @body.
 * The caller must not free @body while the entries are in use.
 */
int tls_parse_certificate_msg(const uint8_t *body, int body_len,
                               struct tls_cert_entry *certs,
                               int max_certs, int *num_certs)
{
	int offset = 0;
	int list_len;
	int cert_count = 0;

	if (!body || !certs || !num_certs)
		return -EINVAL;
	if (body_len < 3 || max_certs <= 0)
		return -EINVAL;

	/* Read certificate_list length (3 bytes, big-endian) */
	list_len = ((int)body[0] << 16) |
	           ((int)body[1] <<  8) |
	            (int)body[2];
	offset = 3;

	if (list_len > body_len - 3)
		return -EINVAL;

	/* Parse each certificate entry */
	while (offset < 3 + list_len && cert_count < max_certs) {
		int cert_data_len;
		int ext_len;

		if (offset + 3 > body_len)
			return -EINVAL;

		/* Certificate data length (3 bytes, big-endian) */
		cert_data_len = ((int)body[offset + 0] << 16) |
		                ((int)body[offset + 1] <<  8) |
		                 (int)body[offset + 2];
		offset += 3;

		if (cert_data_len <= 0 || offset + cert_data_len + 2 > body_len)
			return -EINVAL;

		/* Store pointer to certificate data */
				certs[cert_count].data     = (uint8_t *)(uintptr_t)(body + offset);
		certs[cert_count].data_len = cert_data_len;
		offset += cert_data_len;

		/* Extensions length (2 bytes, big-endian) */
		if (offset + 2 > body_len)
			return -EINVAL;
		ext_len = ((int)body[offset] << 8) | (int)body[offset + 1];
		offset += 2;

		if (ext_len > 0) {
			if (offset + ext_len > body_len)
				return -EINVAL;
						certs[cert_count].extensions = (uint8_t *)(uintptr_t)(body + offset);
			certs[cert_count].ext_len    = ext_len;
			offset += ext_len;
		} else {
			certs[cert_count].extensions = NULL;
			certs[cert_count].ext_len    = 0;
		}

		cert_count++;
	}

	*num_certs = cert_count;
	return (cert_count > 0) ? 0 : -EINVAL;
}
EXPORT_SYMBOL(tls_parse_certificate_msg);

/* ══════════════════════════════════════════════════════════════════════
 * Initialisation
 * ══════════════════════════════════════════════════════════════════════ */

int tls_x509_init(void)
{
	kprintf("[ok] tls: X.509 certificate parser initialised\n");
	return 0;
}
EXPORT_SYMBOL(tls_x509_init);
