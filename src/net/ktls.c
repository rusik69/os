/* ktls.c — Kernel TLS (kTLS) offload to NIC
 *
 * Implements the kTLS offload infrastructure: NIC driver registration,
 * per-connection kTLS context management, and the enable/disable path.
 *
 * When a NIC supports TLS crypto offload, the kTLS subsystem programs
 * the NIC's crypto engine with the negotiated TLS keys.  The NIC then
 * performs record encryption/decryption on the wire, reducing host CPU
 * load.  If no NIC offload is available, the system falls back to
 * software kTLS (record crypto still in the kernel).
 *
 * References:
 *   RFC 8446  — TLS 1.3
 *   RFC 5246  — TLS 1.2
 *   Linux kTLS — Documentation/networking/tls.rst
 */

#include "ktls.h"
#include "tls.h"
#include "tls_aead.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "initcall.h"
#include "export.h"

/* ── offsetof for freestanding kernel ───────────────────────────────── */
#ifndef offsetof
#define offsetof(type, member)  __builtin_offsetof(type, member)
#endif

/* ── Global kTLS State ───────────────────────────────────────────────── */

/* Maximum number of concurrent kTLS contexts */
#define KTLS_MAX_CONTEXTS  256

/* kTLS subsystem lock (protects the context table and offload registry) */
static spinlock_t ktls_lock = SPINLOCK_INIT;

/* Has the subsystem been initialised? */
static int ktls_initialised;

/* Per-NIC offload operation registry.
 * Maps net_device instances to their registered offload callbacks.
 * Indexed by ifindex. */
static const struct ktls_offload_ops *ktls_offload_ops_table[NETDEV_MAX];

/* Dynamic context table for live kTLS connections */
struct ktls_ctx_entry {
	struct ktls_ctx ctx;
	int             in_use;      /* 1 = slot occupied */
};

static struct ktls_ctx_entry *ktls_ctx_table;
static int                    ktls_ctx_count;

/* ── Forward Declarations ──────────────────────────────────────────── */

static struct ktls_ctx *ktls_ctx_alloc(void);
static void            ktls_ctx_free(struct ktls_ctx *ctx);
static int             ktls_offload_hw_enable(struct ktls_ctx *ctx);
static int             ktls_offload_hw_disable(struct ktls_ctx *ctx);
static int             ktls_sw_setup_cipher(struct ktls_ctx *ctx);
static void            ktls_sw_teardown_cipher(struct ktls_ctx *ctx);

/* ── Initialisation ────────────────────────────────────────────────── */

int __init ktls_init(void)
{
	spinlock_acquire(&ktls_lock);
	if (ktls_initialised) {
		spinlock_release(&ktls_lock);
		return 0;
	}

	/* Initialise offload table */
	memset(ktls_offload_ops_table, 0,
	       sizeof(ktls_offload_ops_table));

	/* Allocate context table */
	ktls_ctx_table = (struct ktls_ctx_entry *)
		kmalloc(sizeof(struct ktls_ctx_entry) * KTLS_MAX_CONTEXTS);
	if (!ktls_ctx_table) {
		spinlock_release(&ktls_lock);
		return -ENOMEM;
	}

	memset(ktls_ctx_table, 0,
	       sizeof(struct ktls_ctx_entry) * KTLS_MAX_CONTEXTS);
	ktls_ctx_count = 0;

	ktls_initialised = 1;
	spinlock_release(&ktls_lock);

	kprintf("[OK] ktls: kernel TLS offload subsystem initialised "
	        "(%d context slots)\n", KTLS_MAX_CONTEXTS);

	return 0;
}

subsys_initcall(ktls_init);

/* ── Context Lifecycle (internal) ───────────────────────────────────── */

static struct ktls_ctx *ktls_ctx_alloc(void)
{
	struct ktls_ctx_entry *entry = NULL;

	spinlock_acquire(&ktls_lock);

	for (int i = 0; i < KTLS_MAX_CONTEXTS; i++) {
		if (!ktls_ctx_table[i].in_use) {
			entry = &ktls_ctx_table[i];
			entry->in_use = 1;
			memset(&entry->ctx, 0, sizeof(entry->ctx));
			ktls_ctx_count++;
			break;
		}
	}

	spinlock_release(&ktls_lock);

	return entry ? &entry->ctx : NULL;
}

static void ktls_ctx_free(struct ktls_ctx *ctx)
{
	if (!ctx)
		return;

	/* Compute the table entry index */
	struct ktls_ctx_entry *entry;

	entry = (struct ktls_ctx_entry *)
		((uintptr_t)ctx - offsetof(struct ktls_ctx_entry, ctx));

	spinlock_acquire(&ktls_lock);
	if (entry->in_use) {
		/* Zap crypto material */
		memset(&entry->ctx, 0, sizeof(entry->ctx));
		entry->in_use = 0;
		ktls_ctx_count--;
	}
	spinlock_release(&ktls_lock);
}

/* ── Context Lookup ─────────────────────────────────────────────────── */

/*
 * ktls_ctx_find — Find a kTLS context by owning connection and direction.
 *
 * Scans the context table for a matching (tls_conn, direction) pair.
 * Returns a pointer to the kTLS context, or NULL if not found.
 */
struct ktls_ctx *ktls_ctx_find(const struct tls_conn *conn,
                               int direction)
{
	if (!conn)
		return NULL;
	if (direction != KTLS_TX && direction != KTLS_RX)
		return NULL;

	spinlock_acquire(&ktls_lock);
	for (int i = 0; i < KTLS_MAX_CONTEXTS; i++) {
		struct ktls_ctx_entry *e = &ktls_ctx_table[i];
		if (e->in_use &&
		    e->ctx.tls_conn == conn &&
		    e->ctx.direction == direction) {
			struct ktls_ctx *ctx = &e->ctx;
			spinlock_release(&ktls_lock);
			return ctx;
		}
	}
	spinlock_release(&ktls_lock);
	return NULL;
}
EXPORT_SYMBOL(ktls_ctx_find);

/* ── NIC Offload Registration ───────────────────────────────────────── */

int ktls_offload_register(struct net_device *dev,
                          const struct ktls_offload_ops *ops)
{
	if (!dev || !ops)
		return -EINVAL;

	if (dev->ifindex < 0 || dev->ifindex >= NETDEV_MAX)
		return -EINVAL;

	spinlock_acquire(&ktls_lock);
	ktls_offload_ops_table[dev->ifindex] = ops;
	spinlock_release(&ktls_lock);

	kprintf("[ktls] offload ops registered for %s (ifindex=%d)\n",
	        dev->name, dev->ifindex);

	return 0;
}
EXPORT_SYMBOL(ktls_offload_register);

int ktls_offload_unregister(struct net_device *dev)
{
	if (!dev)
		return -EINVAL;

	if (dev->ifindex < 0 || dev->ifindex >= NETDEV_MAX)
		return -EINVAL;

	spinlock_acquire(&ktls_lock);
	ktls_offload_ops_table[dev->ifindex] = NULL;
	spinlock_release(&ktls_lock);

	kprintf("[ktls] offload ops unregistered for %s (ifindex=%d)\n",
	        dev->name, dev->ifindex);

	return 0;
}
EXPORT_SYMBOL(ktls_offload_unregister);

int ktls_offload_supported(const struct net_device *dev)
{
	int supported = 0;

	if (!dev)
		return 0;
	if (dev->ifindex < 0 || dev->ifindex >= NETDEV_MAX)
		return 0;

	spinlock_acquire(&ktls_lock);
	supported = (ktls_offload_ops_table[dev->ifindex] != NULL) ? 1 : 0;
	spinlock_release(&ktls_lock);

	return supported;
}
EXPORT_SYMBOL(ktls_offload_supported);

/* ── HW Offload Enable / Disable (internal) ────────────────────────── */

static int ktls_offload_hw_enable(struct ktls_ctx *ctx)
{
	const struct ktls_offload_ops *ops;
	int ret;

	if (!ctx || !ctx->dev)
		return -EINVAL;

	if (ctx->dev->ifindex < 0 || ctx->dev->ifindex >= NETDEV_MAX)
		return -EINVAL;

	spinlock_acquire(&ktls_lock);
	ops = ktls_offload_ops_table[ctx->dev->ifindex];
	spinlock_release(&ktls_lock);

	if (!ops)
		return -EOPNOTSUPP;   /* no HW offload available */

	if (!ops->set_key)
		return -EINVAL;

	ret = ops->set_key(ctx->dev, ctx);
	if (ret < 0) {
		kprintf("[ktls] hw_offload: set_key failed for %s: %d\n",
		        ctx->dev->name, ret);
		return ret;
	}

	ctx->offload_type = KTLS_OFFLOAD_HW;
	kprintf("[ktls] hw offload enabled on %s (direction=%s, "
	        "cipher=0x%04x)\n",
	        ctx->dev->name,
	        ctx->direction == KTLS_TX ? "TX" : "RX",
	        ctx->crypto.cipher_suite);

	return 0;
}

static int ktls_offload_hw_disable(struct ktls_ctx *ctx)
{
	const struct ktls_offload_ops *ops;
	int ret;

	if (!ctx || !ctx->dev)
		return -EINVAL;

	if (ctx->dev->ifindex < 0 || ctx->dev->ifindex >= NETDEV_MAX)
		return -EINVAL;

	spinlock_acquire(&ktls_lock);
	ops = ktls_offload_ops_table[ctx->dev->ifindex];
	spinlock_release(&ktls_lock);

	if (!ops || !ops->delete_key)
		return 0;   /* nothing to clean up — not an error */

	ret = ops->delete_key(ctx->dev, ctx);
	if (ret < 0) {
		kprintf("[ktls] hw_offload: delete_key failed for %s: %d\n",
		        ctx->dev->name, ret);
		/* Continue anyway — we want to tear down regardless */
	}

	ctx->offload_type = KTLS_OFFLOAD_NONE;
	return 0;
}

/* ── kTLS Software Path Cipher Setup / Teardown ────────────────────── */

/*
 * ktls_sw_setup_cipher — Copy kTLS crypto keys into the TLS connection's
 * cipher state for software record encryption/decryption.
 *
 * After this call, the connection's wstate (for KTLS_TX) or rstate
 * (for KTLS_RX) is populated with the encryption key, IV, and cipher
 * suite from the kTLS context, and is marked active.
 */
static int ktls_sw_setup_cipher(struct ktls_ctx *ctx)
{
	struct tls_cipher_state *cs;
	struct tls_conn *conn;

	if (!ctx || !ctx->tls_conn)
		return -EINVAL;

	conn = ctx->tls_conn;

	/* Select the cipher state for the appropriate direction */
	if (ctx->direction == KTLS_TX)
		cs = &conn->wstate;
	else
		cs = &conn->rstate;

	/* Zero and populate the cipher state from kTLS crypto info */
	memset(cs, 0, sizeof(*cs));

	/* Copy encryption key */
	memcpy(cs->enc_key, ctx->crypto.enc_key,
	       (size_t)(ctx->crypto.enc_key_len < 32 ?
	                ctx->crypto.enc_key_len : 32));
	cs->enc_key_len = ctx->crypto.enc_key_len;

	/* Salt / implicit IV — stored in ktls_crypto_info.salt,
	 * maps to tls_cipher_state.fixed_iv */
	memcpy(cs->fixed_iv, ctx->crypto.salt,
	       (size_t)(sizeof(ctx->crypto.salt) < sizeof(cs->fixed_iv) ?
	                sizeof(ctx->crypto.salt) : sizeof(cs->fixed_iv)));

	/* Set cipher suite and mark active */
	cs->cipher_suite = ctx->crypto.cipher_suite;
	cs->is_active    = 1;
	cs->seq_num      = 0;

	kprintf("[ktls] SW cipher state setup for connection %p "
	        "(direction=%s, cipher=0x%04x)\n",
	        (void *)conn,
	        ctx->direction == KTLS_TX ? "TX" : "RX",
	        ctx->crypto.cipher_suite);

	return 0;
}

/*
 * ktls_sw_teardown_cipher — Zeroize the TLS connection's cipher state
 * that was set up by ktls_sw_setup_cipher.
 */
static void ktls_sw_teardown_cipher(struct ktls_ctx *ctx)
{
	struct tls_cipher_state *cs;
	struct tls_conn *conn;

	if (!ctx || !ctx->tls_conn)
		return;

	conn = ctx->tls_conn;

	if (ctx->direction == KTLS_TX)
		cs = &conn->wstate;
	else
		cs = &conn->rstate;

	/* Zeroize all crypto material */
	memset(cs->enc_key, 0, sizeof(cs->enc_key));
	cs->enc_key_len = 0;

	memset(cs->fixed_iv, 0, sizeof(cs->fixed_iv));
	memset(cs->explicit_iv, 0, sizeof(cs->explicit_iv));
	memset(cs->mac_key, 0, sizeof(cs->mac_key));
	cs->mac_key_len   = 0;
	cs->seq_num       = 0;
	cs->cipher_suite  = 0;
	cs->is_active     = 0;
}

/* ── kTLS Software Path Encrypt / Decrypt ──────────────────────────── */

/*
 * ktls_sw_encrypt — Encrypt a single TLS record using the kTLS
 * software cipher state.
 *
 * Constructs the TLS record header, AEAD nonce, and AAD from the
 * kTLS context's crypto info, and encrypts the plaintext payload.
 * The output includes the 5-byte record header, ciphertext body,
 * and AEAD authentication tag.
 *
 * On success returns the total bytes written to 'out' (header +
 * ciphertext + tag), or a negative errno on failure.
 */
int ktls_sw_encrypt(struct ktls_ctx *ctx,
                    uint8_t content_type, uint16_t version,
                    const uint8_t *plain, int plain_len,
                    uint8_t *out, int out_cap)
{
	struct tls_record_header *hdr;
	struct tls_aead_ctx aead;
	uint8_t nonce[TLS_AEAD_NONCE_LEN];
	uint8_t aad[13];
	int aad_len;
	uint8_t tag[TLS_AEAD_MAX_TAG_LEN];
	int total_len;
	int ret;

	if (!ctx || !plain || !out)
		return -EINVAL;
	if (plain_len < 0 || plain_len > TLS_MAX_PLAINTEXT_LEN)
		return -EINVAL;

	/* CCS records MUST always be sent in cleartext
	 * (RFC 8446 TLS 1.3 middlebox compat mode).
	 * The kTLS encrypt path must not encrypt them. */
	if (content_type == TLS_CT_CHANGE_CIPHER_SPEC)
		return -EINVAL;

	/* Initialise AEAD from the kTLS crypto info */
	ret = tls_aead_init(&aead, ctx->crypto.enc_key,
	                     ctx->crypto.enc_key_len,
	                     ctx->crypto.cipher_suite);
	if (ret < 0)
		return ret;

	total_len = TLS_RECORD_HEADER_LEN + plain_len + aead.tag_len;
	if (out_cap < total_len)
		return -ENOSPC;

	/* Build the 5-byte record header */
	hdr = (struct tls_record_header *)out;
	hdr->content_type = content_type;
	hdr->version      = htons(version);
	hdr->length       = htons((uint16_t)(plain_len + aead.tag_len));

	/* Build the per-record nonce from kTLS state */
	{
		struct tls_cipher_state fake_cs;

		memset(&fake_cs, 0, sizeof(fake_cs));
		memcpy(fake_cs.fixed_iv, ctx->crypto.salt,
		       sizeof(fake_cs.fixed_iv));
		fake_cs.seq_num = ctx->seq_num;
		tls_aead_build_nonce(&fake_cs, version, nonce);
	}

	/* Build the AAD
	 * TLS 1.2 AAD: seq_num(8 BE) || content_type || version || length
	 * TLS 1.3 AAD: content_type || version || length */
	if (version <= TLS_VER_1_2) {
		uint64_t seq = ctx->seq_num;
		int j;
		for (j = 7; j >= 0; j--) {
			aad[j] = (uint8_t)(seq & 0xFF);
			seq >>= 8;
		}
		aad[8]  = content_type;
		aad[9]  = (uint8_t)((version >> 8) & 0xFF);
		aad[10] = (uint8_t)(version & 0xFF);
		aad[11] = (uint8_t)(((plain_len + aead.tag_len) >> 8) & 0xFF);
		aad[12] = (uint8_t)((plain_len + aead.tag_len) & 0xFF);
		aad_len = 13;
	} else {
		/* TLS 1.3 AAD is shorter — no embedded seq_num */
		aad[0] = content_type;
		aad[1] = (uint8_t)((version >> 8) & 0xFF);
		aad[2] = (uint8_t)(version & 0xFF);
		aad[3] = (uint8_t)(((plain_len + aead.tag_len) >> 8) & 0xFF);
		aad[4] = (uint8_t)((plain_len + aead.tag_len) & 0xFF);
		aad_len = 5;
	}

	/* Encrypt the payload */
	ret = tls_aead_encrypt(&aead, nonce,
	                       aad, aad_len,
	                       plain, plain_len,
	                       out + TLS_RECORD_HEADER_LEN, tag);
	if (ret < 0)
		return ret;

	/* Append the AEAD tag after the ciphertext body */
	memcpy(out + TLS_RECORD_HEADER_LEN + plain_len,
	       tag, (size_t)aead.tag_len);

	/* Advance per-record sequence number */
	ctx->seq_num++;

	return total_len;
}
EXPORT_SYMBOL(ktls_sw_encrypt);

/*
 * ktls_sw_decrypt — Decrypt a single TLS record using the kTLS
 * software cipher state.
 *
 * Strips the 5-byte record header, validates the AEAD tag, and
 * decrypts the payload.  On success returns the plaintext length
 * (bytes written to 'data'), or a negative errno on failure.
 * Returns -EBADMSG on AEAD authentication failure.
 */
int ktls_sw_decrypt(struct ktls_ctx *ctx,
                    const struct tls_record_header *hdr,
                    const uint8_t *cipher, int cipher_len,
                    uint8_t *data, int data_cap)
{
	struct tls_aead_ctx aead;
	uint8_t nonce[TLS_AEAD_NONCE_LEN];
	uint8_t aad[13];
	int aad_len;
	int payload_len;
	int enc_len;
	int tag_len;
	int ret;

	if (!ctx || !hdr || !cipher || !data)
		return -EINVAL;

	payload_len = ntohs(hdr->length);
	if (payload_len < 0 || payload_len > TLS_MAX_CIPHERTEXT_LEN)
		return -EINVAL;

	/* CCS records are NEVER encrypted (RFC 8446 §5.1).
	 * Reject them here before attempting AEAD decryption. */
	if (hdr->content_type == TLS_CT_CHANGE_CIPHER_SPEC)
		return -EINVAL;

	/* Initialise AEAD from the kTLS crypto info */
	ret = tls_aead_init(&aead, ctx->crypto.enc_key,
	                     ctx->crypto.enc_key_len,
	                     ctx->crypto.cipher_suite);
	if (ret < 0)
		return ret;

	tag_len = aead.tag_len;
	if (payload_len < tag_len)
		return -EINVAL;

	/* Split ciphertext into encrypted body and trailing tag */
	enc_len = payload_len - tag_len;
	if (enc_len > data_cap)
		return -ENOSPC;

	{
		const uint8_t *enc_body = cipher;
		const uint8_t *tag = cipher + enc_len;
		uint16_t version = ntohs(hdr->version);

		/* Build the per-record nonce from kTLS state */
		{
			struct tls_cipher_state fake_cs;

			memset(&fake_cs, 0, sizeof(fake_cs));
			memcpy(fake_cs.fixed_iv, ctx->crypto.salt,
			       sizeof(fake_cs.fixed_iv));
			fake_cs.seq_num = ctx->seq_num;
			tls_aead_build_nonce(&fake_cs, version, nonce);
		}

		/* Build the AAD */
		if (version <= TLS_VER_1_2) {
			uint64_t seq = ctx->seq_num;
			int j;
			for (j = 7; j >= 0; j--) {
				aad[j] = (uint8_t)(seq & 0xFF);
				seq >>= 8;
			}
			aad[8]  = hdr->content_type;
			aad[9]  = (uint8_t)((version >> 8) & 0xFF);
			aad[10] = (uint8_t)(version & 0xFF);
			aad[11] = (uint8_t)((payload_len >> 8) & 0xFF);
			aad[12] = (uint8_t)(payload_len & 0xFF);
			aad_len = 13;
		} else {
			aad[0] = hdr->content_type;
			aad[1] = (uint8_t)((version >> 8) & 0xFF);
			aad[2] = (uint8_t)(version & 0xFF);
			aad[3] = (uint8_t)((payload_len >> 8) & 0xFF);
			aad[4] = (uint8_t)(payload_len & 0xFF);
			aad_len = 5;
		}

		/* Decrypt with AEAD (authenticates and decrypts) */
		ret = tls_aead_decrypt(&aead, nonce,
		                       aad, aad_len,
		                       enc_body, enc_len,
		                       tag, data);
		if (ret < 0)
			return ret;
	}

	/* Advance per-record sequence number on success */
	ctx->seq_num++;

	return enc_len;
}
EXPORT_SYMBOL(ktls_sw_decrypt);

/* ── kTLS SW Push (Fragmented Send) ─────────────────────────────────-- */

/*
 * ktls_sw_push — Encrypt and emit one or more TLS records from plaintext.
 *
 * If data_len exceeds TLS_MAX_PLAINTEXT_LEN (16384 B), the data is
 * fragmented into multiple independent records, each encrypted with
 * an independent sequence number from the kTLS context's counter.
 *
 * Returns the total number of bytes written to 'out', or negative errno.
 */
int ktls_sw_push(struct ktls_ctx *ctx,
                 uint8_t content_type,
                 const uint8_t *data, int data_len,
                 uint8_t *out, int out_cap)
{
	int offset = 0;
	int total_written = 0;

	if (!ctx || !data || !out)
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

		ret = ktls_sw_encrypt(ctx, content_type,
		                      ctx->crypto.version,
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
EXPORT_SYMBOL(ktls_sw_push);

/* ── kTLS Enable / Disable (public API) ────────────────────────────── */

int ktls_enable(struct tls_conn *conn, struct net_device *dev,
                const struct ktls_crypto_info *crypto, int direction)
{
	struct ktls_ctx *ctx;
	int ret;

	if (!conn || !crypto)
		return -EINVAL;

	if (direction != KTLS_TX && direction != KTLS_RX)
		return -EINVAL;

	/* Check if kTLS is already enabled for this direction */
	if (ktls_ctx_find(conn, direction)) {
		kprintf("[ktls] kTLS already enabled for connection %p "
		        "(direction=%s)\n",
		        (void *)conn,
		        direction == KTLS_TX ? "TX" : "RX");
		return -EALREADY;
	}

	/* Allocate a kTLS context */
	ctx = ktls_ctx_alloc();
	if (!ctx)
		return -ENOMEM;

	/* Populate the context */
	ctx->direction    = direction;
	ctx->offload_type = KTLS_OFFLOAD_NONE;
	ctx->tls_conn     = conn;
	ctx->dev          = dev;
	ctx->netdev_priv  = NULL;
	memcpy(&ctx->crypto, crypto, sizeof(ctx->crypto));
	ctx->seq_num = 0;

	/* Try HW offload if a NIC device was provided */
	if (dev && ktls_offload_supported(dev)) {
		ret = ktls_offload_hw_enable(ctx);
		if (ret == 0) {
			/* HW offload succeeded — done */
			kprintf("[ktls] enabled HW offload for %s "
			        "(direction=%s)\n",
			        dev->name,
			        direction == KTLS_TX ? "TX" : "RX");
			return 0;
		}

		/* HW offload failed — fall through to software */
		kprintf("[ktls] HW offload not available for %s, "
		        "falling back to SW (ret=%d)\n",
		        dev ? dev->name : "N/A", ret);
	}

	/* Software kTLS — set up the cipher state in the TLS connection
	 * so that the kernel can perform record encryption/decryption
	 * using the negotiated keys. */
	ctx->offload_type = KTLS_OFFLOAD_SW;

	ret = ktls_sw_setup_cipher(ctx);
	if (ret < 0) {
		kprintf("[ktls] SW cipher setup failed: %d\n", ret);
		ktls_ctx_free(ctx);
		return ret;
	}

	kprintf("[ktls] enabled SW kTLS for connection %p "
	        "(direction=%s, cipher=0x%04x)\n",
	        (void *)conn,
	        direction == KTLS_TX ? "TX" : "RX",
	        crypto->cipher_suite);

	return 0;
}
EXPORT_SYMBOL(ktls_enable);

int ktls_disable(struct tls_conn *conn, int direction)
{
	struct ktls_ctx *ctx;

	if (!conn)
		return -EINVAL;

	if (direction != KTLS_TX && direction != KTLS_RX)
		return -EINVAL;

	/* Find the kTLS context for this connection and direction */
	ctx = ktls_ctx_find(conn, direction);
	if (!ctx) {
		kprintf("[ktls] disable: no kTLS context found for "
		        "connection %p (direction=%s)\n",
		        (void *)conn,
		        direction == KTLS_TX ? "TX" : "RX");
		return -ENOENT;
	}

	/* If HW offload was active, tear down NIC state */
	if (ctx->offload_type == KTLS_OFFLOAD_HW && ctx->dev)
		ktls_offload_hw_disable(ctx);

	/* If SW path was active, zeroize cipher state in the connection */
	if (ctx->offload_type == KTLS_OFFLOAD_SW)
		ktls_sw_teardown_cipher(ctx);

	/* Free the kTLS context (zaps all crypto material) */
	ktls_ctx_free(ctx);

	kprintf("[ktls] disabled kTLS for connection %p (direction=%s)\n",
	        (void *)conn,
	        direction == KTLS_TX ? "TX" : "RX");

	return 0;
}
EXPORT_SYMBOL(ktls_disable);

/* ── Resync Helper ──────────────────────────────────────────────────── */

static int ktls_resync_nic(struct net_device *dev, struct ktls_ctx *ctx,
                    uint64_t seq)
{
	const struct ktls_offload_ops *ops;
	int ret;

	if (!dev || !ctx)
		return -EINVAL;

	if (dev->ifindex < 0 || dev->ifindex >= NETDEV_MAX)
		return -EINVAL;

	spinlock_acquire(&ktls_lock);
	ops = ktls_offload_ops_table[dev->ifindex];
	spinlock_release(&ktls_lock);

	if (!ops || !ops->resync)
		return -EOPNOTSUPP;

	ret = ops->resync(dev, ctx, seq);
	if (ret < 0) {
		kprintf("[ktls] resync failed on %s: %d\n",
		        dev->name, ret);
		return ret;
	}

	kprintf("[ktls] NIC resynced to seq %llu on %s\n",
	        (unsigned long long)seq, dev->name);

	return 0;
}
EXPORT_SYMBOL(ktls_resync_nic);

/* ── kTLS SW state query ────────────────────────────────────────────── */

int ktls_sw_is_active(const struct tls_conn *conn, int direction)
{
	struct ktls_ctx *ctx;

	if (!conn)
		return 0;
	if (direction != KTLS_TX && direction != KTLS_RX)
		return 0;

	ctx = ktls_ctx_find(conn, direction);
	if (!ctx)
		return 0;

	return (ctx->offload_type == KTLS_OFFLOAD_SW) ? 1 : 0;
}
EXPORT_SYMBOL(ktls_sw_is_active);

uint64_t ktls_sw_get_seq(const struct tls_conn *conn, int direction)
{
	struct ktls_ctx *ctx;

	if (!conn)
		return 0;
	if (direction != KTLS_TX && direction != KTLS_RX)
		return 0;

	ctx = ktls_ctx_find(conn, direction);
	if (!ctx)
		return 0;

	return ctx->seq_num;
}
EXPORT_SYMBOL(ktls_sw_get_seq);
