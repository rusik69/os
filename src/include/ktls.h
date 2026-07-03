/* ktls.h — Kernel TLS (kTLS) offload to NIC
 *
 * kTLS allows the kernel to handle TLS record encryption/decryption,
 * relieving userspace of this work.  When a NIC supports TLS crypto
 * offload, the kernel can program the NIC's crypto engine to perform
 * TLS record protection on the wire directly, reducing host CPU load.
 *
 * This file defines the kTLS offload infrastructure:
 *   - kTLS crypto info passed to NIC drivers
 *   - kTLS context (per-connection state)
 *   - NIC driver offload operations (set_key, delete_key, resync)
 *   - API to enable/disable kTLS on a connection
 *
 * References:
 *   RFC 8446  — TLS 1.3
 *   RFC 5246  — TLS 1.2
 *   Linux kTLS — Documentation/networking/tls.rst
 */

#ifndef KTLS_H
#define KTLS_H

#include "types.h"
#include "netdevice.h"

/* ── kTLS Offload Types ─────────────────────────────────────────────── */

#define KTLS_OFFLOAD_NONE       0  /* No offload (pure software) */
#define KTLS_OFFLOAD_SW         1  /* Software crypto in kernel */
#define KTLS_OFFLOAD_HW         2  /* Hardware (NIC) offload */

/* ── kTLS Traffic Direction ─────────────────────────────────────────── */

#define KTLS_TX                 0  /* Transmit (encrypt) */
#define KTLS_RX                 1  /* Receive  (decrypt) */

/* ── kTLS Crypto Info (what the NIC needs to program for offload) ───── */

struct ktls_crypto_info {
	uint16_t cipher_suite;         /* TLS cipher suite code point */
	uint8_t  enc_key[32];          /* symmetric encryption key */
	int      enc_key_len;          /* key length in bytes */
	uint8_t  salt[4];              /* implicit IV / salt (TLS 1.2/1.3) */
	uint8_t  rec_seq[8];           /* initial record sequence number (big-endian) */
	uint16_t version;              /* TLS protocol version */
};

/* ── kTLS Context (per-connection, one per direction) ────────────────── */

struct ktls_ctx {
	int              direction;     /* KTLS_TX or KTLS_RX */
	int              offload_type;  /* KTLS_OFFLOAD_* */
	struct ktls_crypto_info crypto; /* crypto parameters for this direction */
	uint64_t         seq_num;       /* current record sequence number */
	void            *netdev_priv;   /* NIC driver private offload handle */
	struct net_device *dev;         /* NIC device for HW offload (may be NULL) */
	struct tls_conn *tls_conn;      /* back-pointer to owning TLS connection */
};

/* ── kTLS NIC Offload Operations ───────────────────────────────────────
 *
 * A NIC driver that supports TLS crypto offload fills in these callbacks
 * and registers them via ktls_offload_register().
 *
 * All callbacks return 0 on success or negative errno on failure.
 */

struct ktls_offload_ops {
	/* resync — called when the NIC needs to re-synchronise its
	 * TLS record sequence number with the kernel's view.
	 * @dev:  the network device
	 * @ctx:  the kTLS context needing resync
	 * @seq:  the expected record sequence number
	 *
	 * The NIC driver should update its internal state so that
	 * the next record it processes uses @seq as the sequence number. */
	int (*resync)(struct net_device *dev, struct ktls_ctx *ctx,
	              uint64_t seq);

	/* set_key — program a new TLS crypto key into the NIC hardware.
	 * @dev:  the network device
	 * @ctx:  the kTLS context with crypto parameters to program
	 *
	 * The NIC driver should extract the encryption key, salt, and
	 * record sequence from ctx->crypto and program them into the
	 * NIC's TLS crypto engine for the given direction. */
	int (*set_key)(struct net_device *dev, struct ktls_ctx *ctx);

	/* delete_key — remove a previously programmed TLS crypto key
	 * from the NIC hardware.
	 * @dev:  the network device
	 * @ctx:  the kTLS context whose key should be removed
	 *
	 * Called when a connection is torn down, so the NIC can free
	 * any hardware resources associated with the key. */
	int (*delete_key)(struct net_device *dev, struct ktls_ctx *ctx);
};

/* ── kTLS API ────────────────────────────────────────────────────────── */

/* Initialise the kTLS subsystem (called once at boot).
 * Returns 0 on success, negative errno on failure. */
int ktls_init(void);

/* Enable kTLS on a given TLS connection for a given direction.
 *
 * @conn:      the TLS connection to enable kTLS on
 * @dev:       the network device (or NULL for software-only)
 * @crypto:    crypto parameters to use for offload
 * @direction: KTLS_TX (encrypt) or KTLS_RX (decrypt)
 *
 * If @dev is non-NULL and supports TLS offload, HW offload is attempted.
 * If @dev is NULL or HW offload is unavailable, software kTLS is used
 * (KTLS_OFFLOAD_SW).
 *
 * Returns 0 on success, negative errno on failure. */
int ktls_enable(struct tls_conn *conn, struct net_device *dev,
                const struct ktls_crypto_info *crypto, int direction);

/* Disable kTLS on a given connection for a given direction.
 * Removes any NIC offload state and zaps crypto material.
 *
 * @conn:      the TLS connection
 * @direction: KTLS_TX or KTLS_RX
 *
 * Returns 0 on success, negative errno on failure. */
int ktls_disable(struct tls_conn *conn, int direction);

/* Register a NIC driver's TLS offload operations.
 * After registration, the kTLS subsystem will use these callbacks
 * when enabling HW kTLS on connections using this device.
 *
 * @dev:  the network device
 * @ops:  the offload callbacks (must remain valid while registered)
 *
 * Returns 0 on success, negative errno on failure. */
int ktls_offload_register(struct net_device *dev,
                          const struct ktls_offload_ops *ops);

/* Unregister a NIC driver's TLS offload operations.
 * After this call, the kTLS subsystem will no longer attempt HW offload
 * on connections using this device (they will fall back to software).
 *
 * @dev:  the network device
 *
 * Returns 0 on success, negative errno on failure. */
int ktls_offload_unregister(struct net_device *dev);

/* Check whether a network device supports HW TLS offload.
 * Returns 1 if HW offload is supported, 0 otherwise. */
int ktls_offload_supported(const struct net_device *dev);

#endif /* KTLS_H */
