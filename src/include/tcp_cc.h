#ifndef TCP_CC_H
#define TCP_CC_H

#include "types.h"

/*
 * tcp_cc.h — Pluggable TCP congestion control framework (cc_ops).
 *
 * Each congestion control algorithm registers a struct cc_ops with
 * function pointers for the CC lifecycle.  The TCP stack dispatches
 * through these ops instead of hardcoding per-algorithm logic.
 *
 * To add a new CC algorithm:
 *   1. Define its per-connection state in tcp_conn (net_internal.h)
 *   2. Implement the internal API (init, update, on_loss, etc.)
 *   3. Assign the next free CC_ALGO_* constant below
 *   4. Add a cc_ops entry during cc_framework_init() in tcp_cc.c
 */

/* ── Algorithm identifiers ──────────────────────────────────────────── */
/* These MUST match the cc_algo field in struct tcp_conn (net_internal.h).
 * Keep in sync! */

#define CC_ALGO_CUBIC      0   /* default: CUBIC (RFC 8312) */
#define CC_ALGO_BBR        1   /* BBR v1 (model-based) */
#define CC_ALGO_BBR3       2   /* BBR v3 (ECN-aware) */
#define CC_ALGO_NEWRENO    3   /* NewReno (RFC 6582) */
#define CC_ALGO_WESTWOOD   4   /* Westwood+ (bandwidth est.) */
#define CC_ALGO_VEGAS      5   /* Vegas (delay-based) */
#define CC_ALGO_HYBLA      6   /* Hybla (satellite links) */
#define CC_ALGO_ILLINOIS   7   /* Illinois (hybrid delay/loss) */
#define CC_ALGO_BIC        8   /* BIC-TCP (binary search) */
#define CC_ALGO_BBR2       9   /* BBR v2 (ECN/probe-RTT) */

#define CC_ALGO_COUNT     10   /* number of defined algorithms */
#define CC_ALGO_MAX       16   /* max registrable (reserved slots) */

/* Forward declaration — struct tcp_conn is defined in net_internal.h.
 * Pointers to it are passed through the cc_ops callbacks as void *sk
 * (Linux-style "sock" convention). */
struct tcp_conn;

/* ── Pluggable congestion control operations ──────────────────────────
 *
 * Each callback takes a void *sk which is a pointer to struct tcp_conn.
 * Not all callbacks are required — set unsupported ones to NULL.
 */

struct cc_ops {
    /* Human-readable name (e.g. "cubic", "bbr", "westwood") */
    const char *name;

    /* ── init ─────────────────────────────────────────────────────
     * Initialise per-connection CC state.  Called when a TCP connection
     * is established or when the CC algorithm is switched via
     * setsockopt(TCP_CONGESTION).
     * Returns 0 on success, negative errno on failure. */
    int (*init)(void *sk);

    /* ── cong_avoid ────────────────────────────────────────────────
     * Called on each ACK during normal operation (not in recovery) to
     * perform congestion window increase.
     * Returns 0 on success, negative errno on failure. */
    int (*cong_avoid)(void *sk);

    /* ── ssthresh ──────────────────────────────────────────────────
     * Called on a congestion event (loss, dupack, RTO, ECN) to compute
     * the new slow-start threshold value (in segments).
     * Returns the new ssthresh value.  The caller applies it. */
    uint32_t (*ssthresh)(void *sk);

    /* ── acked ────────────────────────────────────────────────────
     * Called when new data bytes are ACKed.  @acked is the number of
     * bytes newly ACKed.  Some algorithms (Westwood, Vegas) use this
     * for bandwidth or RTT sampling.
     * Returns 0 on success, negative errno on failure. */
    int (*acked)(void *sk, uint32_t acked);

    /* ── on_loss ───────────────────────────────────────────────────
     * Called on a congestion event (3 dupacks, RTO, loss detection).
     * The algorithm should update its internal state (window reduction,
     * model reset, etc.).  Does NOT modify cwnd or ssthresh directly —
     * those are updated via ssthresh() and the caller's recovery logic. */
    void (*on_loss)(void *sk);

    /* ── get_cwnd ──────────────────────────────────────────────────
     * Return the target congestion window (in segments) recommended
     * by the algorithm.  Used by model-based CCs (BBR, BBRv3) which
     * compute a target cwnd independently of the ACK clock. */
    uint32_t (*get_cwnd)(void *sk);
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Framework API
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Register / unregister ─────────────────────────────────────────── */

/*
 * Register a CC algorithm at the given algorithm slot.
 * @algo  Numeric identifier (CC_ALGO_* constant, must be < CC_ALGO_MAX)
 * @ops   Pointer to a static const struct cc_ops.  Must remain valid
 *        for the lifetime of the system (typically a static const).
 * Returns 0 on success, -EINVAL if @algo >= CC_ALGO_MAX or @ops is NULL,
 *         -EEXIST if a different ops is already registered at this slot.
 *
 * It is safe to re-register the exact same ops pointer (idempotent).
 */
int cc_register(uint8_t algo, const struct cc_ops *ops);

/*
 * Unregister a CC algorithm.  Returns 0 on success,
 * -ENOENT if no algorithm is registered at @algo.
 */
int cc_unregister(uint8_t algo);

/* ── Dispatch functions ────────────────────────────────────────────── */

/*
 * Call init() for algorithm @algo on connection @sk.
 * Returns 0 on success, -EINVAL if @algo out of range or no ops registered.
 */
int cc_init_conn(uint8_t algo, void *sk);

/*
 * Call cong_avoid() for algorithm @algo on connection @sk.
 * Returns 0 on success, -EINVAL on error.
 */
int cc_cong_avoid(uint8_t algo, void *sk);

/*
 * Call ssthresh() for algorithm @algo on connection @sk.
 * Returns new ssthresh value, or 2 (minimum safe) if not registered.
 */
uint32_t cc_ssthresh(uint8_t algo, void *sk);

/*
 * Call acked() for algorithm @algo on connection @sk.
 * @acked  Number of bytes newly ACKed.
 * Returns 0 on success, -EINVAL on error.
 */
int cc_acked(uint8_t algo, void *sk, uint32_t acked);

/*
 * Call on_loss() for algorithm @algo on connection @sk.
 */
void cc_on_loss(uint8_t algo, void *sk);

/*
 * Call get_cwnd() for algorithm @algo on connection @sk.
 * Returns target cwnd, or 10 (default) if not registered.
 */
uint32_t cc_get_cwnd(uint8_t algo, void *sk);

/*
 * Return the human-readable name of algorithm @algo, or "unknown"
 * if @algo is not registered or out of range.
 */
const char *cc_name(uint8_t algo);

/* ── Initialisation ────────────────────────────────────────────────── */

/*
 * Initialise the CC framework: zero the registration table, then
 * register all built-in congestion control algorithms.
 * Called once at boot from net_init() or a module init function.
 */
void cc_framework_init(void);

#endif /* TCP_CC_H */
