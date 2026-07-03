/* tls_x509.h — X.509 Certificate parsing and TLS Certificate message API
 *
 * Implements DER-encoded X.509v3 certificate parsing (RFC 5280)
 * and the TLS Certificate handshake message (RFC 8446 §4.4.2).
 *
 * This is a minimal skeleton — enough to parse self-signed certificates
 * and extract the public key for TLS handshake verification.  Full
 * chain validation, CRL/OCSP checking, and extended key usage are
 * reserved for future enhancement.
 */

#ifndef TLS_X509_H
#define TLS_X509_H

#include "types.h"
#include "sha256.h"
#include "tls.h"

/* ── DER Tag Constants (ITU-T X.690 §8) ────────────────────────── */

#define DER_TAG_BOOLEAN         0x01
#define DER_TAG_INTEGER          0x02
#define DER_TAG_BIT_STRING       0x03
#define DER_TAG_OCTET_STRING     0x04
#define DER_TAG_NULL             0x05
#define DER_TAG_OID              0x06
#define DER_TAG_UTF8_STRING      0x0C
#define DER_TAG_PRINTABLE_STRING 0x13
#define DER_TAG_IA5_STRING       0x16
#define DER_TAG_UTC_TIME         0x17
#define DER_TAG_GENERALIZED_TIME 0x18
#define DER_TAG_SEQUENCE         0x30
#define DER_TAG_SET              0x31

/* Context-specific constructed tags */
#define DER_TAG_CTX_0           0xA0
#define DER_TAG_CTX_1           0xA1
#define DER_TAG_CTX_2           0xA2
#define DER_TAG_CTX_3           0xA3

/* ── Well-Known OIDs (DER-encoded) ─────────────────────────────── */

/* RSA with SHA-256 (1.2.840.113549.1.1.11) */
#define OID_RSA_SHA256_BYTES \
	0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B
#define OID_RSA_SHA256_LEN 9

/* RSA encryption (1.2.840.113549.1.1.1) */
#define OID_RSA_ENCRYPTION_BYTES \
	0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01
#define OID_RSA_ENCRYPTION_LEN 9

/* ECDSA with SHA-256 (1.2.840.10045.4.3.2) */
#define OID_ECDSA_SHA256_BYTES \
	0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02
#define OID_ECDSA_SHA256_LEN 8

/* EC public key (1.2.840.10045.2.1) */
#define OID_EC_PUBLIC_KEY_BYTES \
	0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01
#define OID_EC_PUBLIC_KEY_LEN 7

/* ── X.509 Certificate Structures ─────────────────────────────── */

#define X509_SERIAL_MAX_LEN   64
#define X509_DN_MAX_LEN       256
#define X509_PUBKEY_MAX_LEN   1024
#define X509_SIG_VALUE_MAX_LEN 512

/* Distinguished Name (RFC 4514) — stored as raw encoded string */
struct tls_x509_dn {
	uint8_t  raw[X509_DN_MAX_LEN];
	int      raw_len;
};

/* AlgorithmIdentifier (RFC 5280 §4.1.1.2) */
struct tls_x509_algo_id {
	uint8_t  oid[16];
	int      oid_len;
	uint8_t  params[32];
	int      params_len;
};

/* SubjectPublicKeyInfo (RFC 5280 §4.1.1.2) */
struct tls_x509_pubkey {
	struct tls_x509_algo_id algorithm;
	uint8_t  key_data[X509_PUBKEY_MAX_LEN];
	int      key_data_len;
};

/* Validity period (RFC 5280 §4.1.2.5) */
struct tls_x509_validity {
	uint8_t  not_before[16];  /* raw DER-encoded Time */
	int      not_before_len;
	uint8_t  not_after[16];
	int      not_after_len;
};

/* Parsed X.509 certificate */
struct tls_x509_cert {
	/* TBSCertificate fields */
	int             version;         /* 1, 2, or 3 (v1, v2, v3) */
	uint8_t         serial[X509_SERIAL_MAX_LEN];
	int             serial_len;
	struct tls_x509_algo_id signature_algo;
	struct tls_x509_dn  issuer;
	struct tls_x509_validity validity;
	struct tls_x509_dn  subject;
	struct tls_x509_pubkey pubkey;

	/* Raw DER offsets (for signature verification) */
	const uint8_t  *tbs_der;         /* pointer into the source DER */
	int             tbs_len;         /* length of TBSCertificate DER */

	/* Signature */
	struct tls_x509_algo_id sig_algo;
	uint8_t         sig_value[X509_SIG_VALUE_MAX_LEN];
	int             sig_value_len;

	/* SHA-256 fingerprint of the DER-encoded certificate */
	uint8_t         fingerprint[SHA256_DIGEST_SIZE];
};

/* ── DER Parsing Helpers ────────────────────────────────────────── */

/**
 * x509_parse_der_length — Parse a DER length field.
 * @der:   Pointer to the length field (must not be NULL).
 * @len:   On success, set to the parsed length value.
 *
 * Returns the number of bytes consumed by the length field (1 or more),
 * or a negative errno on failure.
 */
int x509_parse_der_length(const uint8_t *der, uint32_t *len);

/**
 * x509_skip_der_tlv — Get the total size of a DER TLV.
 * @der:   Pointer to the start of the TLV.
 * @size:  Remaining bytes available.
 *
 * Returns the offset past the TLV (tag + length + value), or
 * a negative errno on failure.
 */
int x509_skip_der_tlv(const uint8_t *der, int size);

/**
 * x509_read_der_tlv — Parse a DER TLV.
 * @der:   Pointer to the start of the TLV.
 * @size:  Remaining bytes available.
 * @tag:   Set to the tag byte.
 * @value: Set to a pointer inside @der pointing to the value.
 * @vlen:  Set to the length of the value.
 *
 * Returns the total TLV size (tag + length + value), or a negative
 * errno on failure.
 */
int x509_read_der_tlv(const uint8_t *der, int size,
                       uint8_t *tag, const uint8_t **value, uint32_t *vlen);

/**
 * x509_match_oid — Check if an OID at a DER offset matches a known one.
 * @der:   DER buffer.
 * @offset: Offset into @der where the OID tag should be.
 * @size:  Remaining bytes.
 * @oid:   Expected OID bytes (tag + length + value expected).
 * @oid_len: Length of the expected OID in bytes.
 *
 * Returns 1 if the OID at @offset matches, 0 if not, or a negative
 * errno on invalid DER.
 */
int x509_match_oid(const uint8_t *der, int offset, int size,
                    const uint8_t *oid, int oid_len);

/* ── X.509 Certificate Parsing ─────────────────────────────────── */

/**
 * x509_parse_cert — Parse a single DER-encoded X.509 certificate.
 * @der:      Pointer to the DER-encoded certificate.
 * @der_len:  Length of the DER data.
 * @cert:     Output structure to fill.
 *
 * Parses the certificate, extracting all relevant fields and computing
 * the SHA-256 fingerprint.  Does not validate the signature.
 *
 * Returns 0 on success, or a negative errno on parse failure.
 */
int x509_parse_cert(const uint8_t *der, int der_len,
                     struct tls_x509_cert *cert);

/**
 * x509_print_cert — Log a human-readable summary of a parsed cert.
 * @cert:    Parsed certificate.
 * @prefix:  Prefix string for log output (e.g. "server").
 */
void x509_print_cert(const struct tls_x509_cert *cert, const char *prefix);

/* ── Initialisation ─────────────────────────────────────────────── */

/**
 * tls_x509_init — Initialise the X.509 certificate subsystem.
 * Called once at boot from tls_init().
 * Returns 0 on success, negative errno on failure.
 */
int tls_x509_init(void);

#endif /* TLS_X509_H */
