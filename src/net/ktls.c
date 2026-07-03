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

	/* Software kTLS — the record encryption/decryption will be
	 * handled by the kernel TLS layer (tls_record_encrypt /
	 * tls_record_decrypt) using the configured cipher state.
	 * Full software path implementation is in task 11. */
	ctx->offload_type = KTLS_OFFLOAD_SW;

	kprintf("[ktls] enabled SW kTLS for connection (direction=%s, "
	        "cipher=0x%04x)\n",
	        direction == KTLS_TX ? "TX" : "RX",
	        crypto->cipher_suite);

	return 0;
}
EXPORT_SYMBOL(ktls_enable);

int ktls_disable(struct tls_conn *conn, int direction)
{
	/* Note: in a full implementation we would locate the kTLS context
	 * by searching the context table for a matching tls_conn + direction.
	 * For now we rely on the caller having already tracked the ctx
	 * pointer — this is a simplifying stub for the offload infrastructure.
	 *
	 * The software path (ktls.c) will be expanded in task 11 to provide
	 * full context lookup and teardown. */

	if (!conn)
		return -EINVAL;

	if (direction != KTLS_TX && direction != KTLS_RX)
		return -EINVAL;

	kprintf("[ktls] disabled kTLS for connection (direction=%s)\n",
	        direction == KTLS_TX ? "TX" : "RX");

	return 0;
}
EXPORT_SYMBOL(ktls_disable);

/* ── Resync Helper ──────────────────────────────────────────────────── */

int ktls_resync_nic(struct net_device *dev, struct ktls_ctx *ctx,
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
