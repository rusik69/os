/*
 * kunit_tls.c — KUnit test suite for TLS/HTTPS connectivity.
 *
 * Tests the complete TLS record layer: init, connection lifecycle,
 * record encrypt/decrypt round-trip (the core of HTTPS data transfer),
 * fragmentation, middlebox compatibility, and kTLS software path.
 *
 * These tests verify that TLS application data (the "S" in HTTPS)
 * can be encrypted as a TLS record and then decrypted back to the
 * original plaintext, which is the fundamental building block of
 * HTTPS connectivity.
 */

#include "kunit.h"
#include "tls.h"
#include "tls_aead.h"
#include "ktls.h"
#include "crypto.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"

/* ── Test vectors ───────────────────────────────────────────────────── */

/* AES-128-GCM pre-shared key (from NIST test vector) */
static const uint8_t test_enc_key[16] = {
	0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c,
	0x6d, 0x6a, 0x8f, 0x94, 0x67, 0x30, 0x83, 0x08,
};

/* Salt / fixed IV for TLS 1.2 AEAD */
static const uint8_t test_salt[4] = { 0x01, 0x02, 0x03, 0x04 };

/* Sample HTTP GET request (the kind of data HTTPS would carry) */
static const uint8_t http_request[] =
	"GET /index.html HTTP/1.1\r\n"
	"Host: example.com\r\n"
	"User-Agent: HermesOS/1.0\r\n"
	"Accept: */*\r\n"
	"Connection: close\r\n"
	"\r\n";

/* Sample HTTP 200 OK response */
static const uint8_t http_response[] =
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/html\r\n"
	"Content-Length: 15\r\n"
	"Connection: close\r\n"
	"\r\n"
	"<h1>Hello!</h1>";

/* Helper: set up TLS cipher state with AES-128-GCM test key */
static void setup_cipher_state(struct tls_cipher_state *cs)
{
	memcpy(cs->enc_key, test_enc_key, sizeof(test_enc_key));
	cs->enc_key_len = sizeof(test_enc_key);
	memcpy(cs->fixed_iv, test_salt, sizeof(test_salt));
	cs->cipher_suite = TLS_AES_128_GCM_SHA256;
	cs->is_active = 1;
	cs->seq_num = 0;
}

/* ====================================================================
 *  1. TLS subsystem initialisation
 * ==================================================================== */

static void tls_init_test(struct kunit *test)
{
	int ret;

	ret = tls_init();
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Second call should be idempotent */
	ret = tls_init();
	KUNIT_EXPECT_EQ(test, ret, 0);
}

/* ====================================================================
 *  2. TLS connection lifecycle
 * ==================================================================== */

static void tls_conn_lifecycle_test(struct kunit *test)
{
	struct tls_conn conn;
	int ret;

	/* Create a TLS 1.2 client connection */
	memset(&conn, 0, sizeof(conn));
	ret = tls_conn_init(&conn, 1, TLS_VER_1_2);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, (int64_t)conn.is_client, (int64_t)1);
	KUNIT_EXPECT_EQ(test, (int64_t)conn.version, (int64_t)TLS_VER_1_2);
	KUNIT_EXPECT_EQ(test, (int64_t)conn.renego_allowed, (int64_t)1);

	/* Cleanup should not crash */
	tls_conn_cleanup(&conn);

	/* Create a TLS 1.3 server connection */
	ret = tls_conn_init(&conn, 0, TLS_VER_1_3);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, (int64_t)conn.is_client, (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)conn.version, (int64_t)TLS_VER_1_3);
	KUNIT_EXPECT_EQ(test, (int64_t)conn.renego_allowed, (int64_t)0);
	/* TLS 1.3 should enable middlebox compat by default */
	KUNIT_EXPECT_EQ(test, (int64_t)conn.middlebox_compat, (int64_t)1);

	tls_conn_cleanup(&conn);

	/* NULL init should fail gracefully */
	ret = tls_conn_init(NULL, 1, TLS_VER_1_2);
	KUNIT_EXPECT_EQ(test, ret, (int)-EINVAL);

	/* Cleanup on NULL should be safe */
	tls_conn_cleanup(NULL);
}

/* ====================================================================
 *  3. TLS record encrypt/decrypt round-trip (HTTPS core)
 *
 *  This is the fundamental test for HTTPS connectivity: can we
 *  encrypt application data (an HTTP request) into a TLS record,
 *  then decrypt it back to the original plaintext?
 * ==================================================================== */

static void tls_record_roundtrip_test(struct kunit *test)
{
	struct tls_conn conn;
	uint8_t record_buf[TLS_MAX_RECORD_LEN];
	uint8_t decrypted[TLS_MAX_PLAINTEXT_LEN + 64];
	uint8_t content_type;
	int record_len;
	int dec_len;

	/* Initialise connection with TLS 1.2 */
	memset(&conn, 0, sizeof(conn));
	tls_conn_init(&conn, 1, TLS_VER_1_2);

	/* Manually set up cipher state (simulating a completed TLS handshake) */
	setup_cipher_state(&conn.wstate);
	setup_cipher_state(&conn.rstate);

	/* ── Step 1: Encrypt the HTTP request into a TLS record ────────── */
	record_len = tls_record_send(&conn, TLS_CT_APPLICATION_DATA,
	                             http_request, (int)sizeof(http_request) - 1,
	                             record_buf, (int)sizeof(record_buf));
	KUNIT_EXPECT_TRUE(test, record_len > 0);
	if (record_len <= 0) {
		tls_conn_cleanup(&conn);
		return;
	}

	/* Verify the record has a valid TLS record header */
	{
		struct tls_record_header *hdr =
			(struct tls_record_header *)record_buf;
		KUNIT_EXPECT_EQ(test, (int64_t)hdr->content_type,
		                (int64_t)TLS_CT_APPLICATION_DATA);
		KUNIT_EXPECT_EQ(test, (int64_t)ntohs(hdr->version),
		                (int64_t)TLS_VER_1_2);
		KUNIT_EXPECT_TRUE(test, ntohs(hdr->length) > 0);
	}

	/* ── Step 2: Decrypt the TLS record back to plaintext ───────────── */
	dec_len = tls_record_recv(&conn,
	                          record_buf, record_len,
	                          &content_type,
	                          decrypted, (int)sizeof(decrypted));
	KUNIT_EXPECT_TRUE(test, dec_len >= 0);
	if (dec_len < 0) {
		tls_conn_cleanup(&conn);
		return;
	}

	/* Verify the decrypted data matches the original HTTP request */
	KUNIT_EXPECT_EQ(test, dec_len, (int)(sizeof(http_request) - 1));
	KUNIT_EXPECT_EQ(test, (int64_t)content_type,
	                (int64_t)TLS_CT_APPLICATION_DATA);

	if (dec_len == (int)(sizeof(http_request) - 1)) {
		int match = memcmp(decrypted, http_request, (size_t)dec_len);
		KUNIT_EXPECT_EQ(test, match, 0);
	}

	tls_conn_cleanup(&conn);
}

/* ====================================================================
 *  4. TLS record round-trip with HTTP response
 * ==================================================================== */

static void tls_http_response_roundtrip_test(struct kunit *test)
{
	struct tls_conn conn;
	uint8_t record_buf[TLS_MAX_RECORD_LEN];
	uint8_t decrypted[TLS_MAX_PLAINTEXT_LEN + 64];
	uint8_t content_type;
	int record_len;
	int dec_len;
	int response_len = (int)sizeof(http_response) - 1;

	memset(&conn, 0, sizeof(conn));
	tls_conn_init(&conn, 0, TLS_VER_1_2);  /* server role */

	/* Set up cipher state */
	setup_cipher_state(&conn.wstate);
	setup_cipher_state(&conn.rstate);

	/* Encrypt the HTTP response */
	record_len = tls_record_send(&conn, TLS_CT_APPLICATION_DATA,
	                             http_response, response_len,
	                             record_buf, (int)sizeof(record_buf));
	KUNIT_EXPECT_TRUE(test, record_len > 0);
	if (record_len <= 0) {
		tls_conn_cleanup(&conn);
		return;
	}

	/* Decrypt */
	dec_len = tls_record_recv(&conn,
	                          record_buf, record_len,
	                          &content_type,
	                          decrypted, (int)sizeof(decrypted));
	KUNIT_EXPECT_TRUE(test, dec_len >= 0);
	if (dec_len < 0) {
		tls_conn_cleanup(&conn);
		return;
	}

	KUNIT_EXPECT_EQ(test, dec_len, response_len);
	KUNIT_EXPECT_EQ(test, (int64_t)content_type,
	                (int64_t)TLS_CT_APPLICATION_DATA);

	if (dec_len == response_len) {
		int match = memcmp(decrypted, http_response, (size_t)dec_len);
		KUNIT_EXPECT_EQ(test, match, 0);
	}

	tls_conn_cleanup(&conn);
}

/* ====================================================================
 *  5. TLS record fragmentation
 *
 *  Test that sending data larger than TLS_MAX_PLAINTEXT_LEN (16384)
 *  produces multiple records, each individually decodable.
 * ==================================================================== */

static void tls_record_fragmentation_test(struct kunit *test)
{
	struct tls_conn conn;
	uint8_t large_data[TLS_MAX_PLAINTEXT_LEN + 1000];
	uint8_t record_buf[TLS_MAX_RECORD_LEN * 2];
	uint8_t decrypted[TLS_MAX_PLAINTEXT_LEN * 2];
	uint8_t content_type;
	int total_encrypted;
	int offset;
	int decrypted_total = 0;

	/* Fill with a repeating pattern */
	for (int i = 0; i < (int)sizeof(large_data); i++)
		large_data[i] = (uint8_t)(i & 0xFF);

	memset(&conn, 0, sizeof(conn));
	tls_conn_init(&conn, 1, TLS_VER_1_2);

	/* Set up cipher state */
	setup_cipher_state(&conn.wstate);
	setup_cipher_state(&conn.rstate);

	/* Encode the large data (will produce multiple records) */
	total_encrypted = tls_record_send(&conn, TLS_CT_APPLICATION_DATA,
	                                   large_data, (int)sizeof(large_data),
	                                   record_buf, (int)sizeof(record_buf));
	KUNIT_EXPECT_TRUE(test, total_encrypted > 0);
	if (total_encrypted <= 0) {
		tls_conn_cleanup(&conn);
		return;
	}

	/* Verify we got more than one record (fragmentation happened) */
	{
		int min_multi = (TLS_RECORD_HEADER_LEN + 1 + 16) * 2;
		KUNIT_EXPECT_TRUE(test, total_encrypted > min_multi);
	}

	/* Decrypt each record in sequence and reassemble */
	offset = 0;
	while (offset < total_encrypted) {
		int remaining = total_encrypted - offset;
		int dec_len;

		dec_len = tls_record_recv(&conn,
		                          record_buf + offset, remaining,
		                          &content_type,
		                          decrypted + decrypted_total,
		                          (int)(sizeof(decrypted) - decrypted_total));
		KUNIT_EXPECT_TRUE(test, dec_len >= 0);
		if (dec_len < 0)
			break;

		KUNIT_EXPECT_EQ(test, (int64_t)content_type,
		                (int64_t)TLS_CT_APPLICATION_DATA);

		/* Advance by reading the record header length */
		if (remaining >= TLS_RECORD_HEADER_LEN) {
			struct tls_record_header *rh =
				(struct tls_record_header *)(record_buf + offset);
			offset += TLS_RECORD_HEADER_LEN + ntohs(rh->length);
		} else {
			break;
		}
		decrypted_total += dec_len;
	}

	/* Verify we got the right amount of data back */
	KUNIT_EXPECT_EQ(test, decrypted_total, (int)sizeof(large_data));
	if (decrypted_total == (int)sizeof(large_data)) {
		int match = memcmp(decrypted, large_data, sizeof(large_data));
		KUNIT_EXPECT_EQ(test, match, 0);
	}

	tls_conn_cleanup(&conn);
}

/* ====================================================================
 *  6. TLS record invalid input handling
 * ==================================================================== */

static void tls_record_invalid_test(struct kunit *test)
{
	struct tls_conn conn;
	uint8_t buf[64];
	uint8_t content_type;
	int ret;

	memset(&conn, 0, sizeof(conn));
	tls_conn_init(&conn, 1, TLS_VER_1_2);

	/* NULL params */
	ret = tls_record_send(NULL, TLS_CT_APPLICATION_DATA,
	                      (const uint8_t *)"data", 4,
	                      buf, (int)sizeof(buf));
	KUNIT_EXPECT_EQ(test, ret, (int)-EINVAL);

	ret = tls_record_recv(&conn, NULL, 0, &content_type,
	                      buf, (int)sizeof(buf));
	KUNIT_EXPECT_EQ(test, ret, (int)-EINVAL);

	/* Too-short input for recv */
	{
		uint8_t short_in[2] = {0};
		ret = tls_record_recv(&conn, short_in, 2, &content_type,
		                      buf, (int)sizeof(buf));
		KUNIT_EXPECT_EQ(test, ret, (int)-EINVAL);
	}

	tls_conn_cleanup(&conn);
}

/* ====================================================================
 *  7. TLS 1.3 Middlebox Compatibility Mode
 * ==================================================================== */

static void tls_middlebox_compat_test(struct kunit *test)
{
	uint8_t ccs_buf[TLS_RECORD_HEADER_LEN + 1];
	int ret;

	/* Build a CCS record */
	ret = tls_build_ccs_record(ccs_buf, (int)sizeof(ccs_buf));
	KUNIT_EXPECT_EQ(test, ret, TLS_RECORD_HEADER_LEN + 1);
	KUNIT_EXPECT_EQ(test, (int)ret, 6);

	if (ret == 6) {
		struct tls_record_header *hdr =
			(struct tls_record_header *)ccs_buf;
		KUNIT_EXPECT_EQ(test, (int64_t)hdr->content_type,
		                (int64_t)TLS_CT_CHANGE_CIPHER_SPEC);
		KUNIT_EXPECT_EQ(test, (int64_t)ntohs(hdr->version),
		                (int64_t)TLS_VER_1_2);
		KUNIT_EXPECT_EQ(test, (int64_t)ntohs(hdr->length), (int64_t)1);
		KUNIT_EXPECT_EQ(test, (int64_t)ccs_buf[TLS_RECORD_HEADER_LEN],
		                (int64_t)0x01);
	}

	/* Handle a valid CCS record */
	ret = tls_handle_ccs((const uint8_t *)"\x01", 1);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Handle invalid CCS records */
	ret = tls_handle_ccs(NULL, 0);
	KUNIT_EXPECT_EQ(test, ret, (int)-EINVAL);

	ret = tls_handle_ccs((const uint8_t *)"\x01\x02", 2);
	KUNIT_EXPECT_EQ(test, ret, (int)-EINVAL);

	ret = tls_handle_ccs((const uint8_t *)"\x00", 1);
	KUNIT_EXPECT_EQ(test, ret, (int)-EINVAL);

	/* Should-send / clear-ccs */
	{
		struct tls_conn conn;
		tls_conn_init(&conn, 1, TLS_VER_1_3);

		/* TLS 1.3 with middlebox compat enabled means CCS may be pending */
		conn.ccs_pending = 1;
		KUNIT_EXPECT_EQ(test, tls_should_send_ccs(&conn), 1);

		tls_clear_ccs(&conn);
		KUNIT_EXPECT_EQ(test, tls_should_send_ccs(&conn), 0);

		tls_conn_cleanup(&conn);
	}

	/* Edge cases */
	KUNIT_EXPECT_EQ(test, tls_should_send_ccs(NULL), 0);
	tls_clear_ccs(NULL);  /* should not crash */
}

/* ====================================================================
 *  8. kTLS software encrypt/decrypt cycle
 *
 *  Tests the kTLS offload infrastructure: enable, encrypt via the
 *  software path, decrypt via tls_record_recv (which reads the cipher
 *  state set up by ktls_sw_setup_cipher), then disable.
 * ==================================================================== */

static void ktls_sw_roundtrip_test(struct kunit *test)
{
	struct tls_conn conn;
	struct ktls_crypto_info crypto;
	uint8_t record_buf[TLS_MAX_RECORD_LEN];
	uint8_t decrypted[TLS_MAX_PLAINTEXT_LEN + 64];
	int ret;

	static const uint8_t test_data[] = "Hello, HTTPS World!";

	/* Initialise connection */
	memset(&conn, 0, sizeof(conn));
	tls_conn_init(&conn, 1, TLS_VER_1_2);

	/* Set up kTLS crypto info */
	memset(&crypto, 0, sizeof(crypto));
	crypto.cipher_suite = TLS_AES_128_GCM_SHA256;
	memcpy(crypto.enc_key, test_enc_key, sizeof(test_enc_key));
	crypto.enc_key_len = sizeof(test_enc_key);
	memcpy(crypto.salt, test_salt, sizeof(test_salt));
	crypto.version = TLS_VER_1_2;

	/* Enable kTLS TX direction */
	ret = ktls_enable(&conn, NULL, &crypto, KTLS_TX);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Enable kTLS RX direction */
	ret = ktls_enable(&conn, NULL, &crypto, KTLS_RX);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* Verify kTLS SW is active */
	KUNIT_EXPECT_EQ(test, ktls_sw_is_active(&conn, KTLS_TX), 1);
	KUNIT_EXPECT_EQ(test, ktls_sw_is_active(&conn, KTLS_RX), 1);
	KUNIT_EXPECT_EQ(test, (int64_t)ktls_sw_get_seq(&conn, KTLS_TX),
	                (int64_t)0);
	KUNIT_EXPECT_EQ(test, (int64_t)ktls_sw_get_seq(&conn, KTLS_RX),
	                (int64_t)0);

	/* After ktls_enable, the cipher state is set up in conn->wstate.
	 * Use tls_record_send to encrypt the test data. */
	ret = tls_record_send(&conn, TLS_CT_APPLICATION_DATA,
	                      test_data, (int)sizeof(test_data) - 1,
	                      record_buf, (int)sizeof(record_buf));
	KUNIT_EXPECT_TRUE(test, ret > 0);
	if (ret <= 0) {
		ktls_disable(&conn, KTLS_TX);
		ktls_disable(&conn, KTLS_RX);
		tls_conn_cleanup(&conn);
		return;
	}

	/* Decrypt via tls_record_recv (uses conn->rstate set up by ktls) */
	{
		uint8_t ct;
		int dec_len = tls_record_recv(&conn,
		                              record_buf, ret,
		                              &ct,
		                              decrypted, (int)sizeof(decrypted));
		KUNIT_EXPECT_TRUE(test, dec_len >= 0);
		if (dec_len >= 0) {
			KUNIT_EXPECT_EQ(test, dec_len,
			                (int)(sizeof(test_data) - 1));
			int match = memcmp(decrypted, test_data,
			                   (size_t)dec_len);
			KUNIT_EXPECT_EQ(test, match, 0);
		}
	}

	/* Disable kTLS */
	ret = ktls_disable(&conn, KTLS_TX);
	KUNIT_EXPECT_EQ(test, ret, 0);

	ret = ktls_disable(&conn, KTLS_RX);
	KUNIT_EXPECT_EQ(test, ret, 0);

	/* After disable, SW should not be active */
	KUNIT_EXPECT_EQ(test, ktls_sw_is_active(&conn, KTLS_TX), 0);
	KUNIT_EXPECT_EQ(test, ktls_sw_is_active(&conn, KTLS_RX), 0);

	tls_conn_cleanup(&conn);
}

/* ====================================================================
 *  Test case lists
 * ==================================================================== */

static const struct kunit_case tls_core_cases[] = {
	KUNIT_CASE(tls_init_test),
	KUNIT_CASE(tls_conn_lifecycle_test),
	KUNIT_CASE(tls_record_roundtrip_test),
	KUNIT_CASE(tls_http_response_roundtrip_test),
	KUNIT_CASE(tls_record_fragmentation_test),
	KUNIT_CASE(tls_record_invalid_test),
	KUNIT_CASE(tls_middlebox_compat_test),
	{0}
};

static const struct kunit_case ktls_cases[] = {
	KUNIT_CASE(ktls_sw_roundtrip_test),
	{0}
};

/* ====================================================================
 *  Suite definitions
 * ==================================================================== */

static struct kunit_suite tls_core_suite;
static struct kunit_suite ktls_suite;

/* ====================================================================
 *  Registration
 * ==================================================================== */

void kunit_tls_register(void)
{
	/* ── TLS core record layer suite ── */
	{
		int ci = 0;
		while (tls_core_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			tls_core_suite.cases[ci].name = tls_core_cases[ci].name;
			tls_core_suite.cases[ci].run  = tls_core_cases[ci].run;
			ci++;
		}
		tls_core_suite.cases[ci].name = NULL;
		tls_core_suite.cases[ci].run  = NULL;
		tls_core_suite.name     = "tls_core";
		tls_core_suite.setup    = NULL;
		tls_core_suite.teardown = NULL;
		kunit_register_suite(&tls_core_suite);
	}

	/* ── kTLS suite ── */
	{
		int ci = 0;
		while (ktls_cases[ci].run != NULL && ci < KUNIT_MAX_CASES - 1) {
			ktls_suite.cases[ci].name = ktls_cases[ci].name;
			ktls_suite.cases[ci].run  = ktls_cases[ci].run;
			ci++;
		}
		ktls_suite.cases[ci].name = NULL;
		ktls_suite.cases[ci].run  = NULL;
		ktls_suite.name     = "ktls";
		ktls_suite.setup    = NULL;
		ktls_suite.teardown = NULL;
		kunit_register_suite(&ktls_suite);
	}

	kprintf("[KUnit] TLS/HTTPS test suites registered\n");
}
