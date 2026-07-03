#ifndef PKT_SCHED_H
#define PKT_SCHED_H

#include "types.h"

/* Qdisc types */
#define QDISC_PFIFO_FAST  0
#define QDISC_FQ_CODEL    1
#define QDISC_HTB         2
#define QDISC_TBF         3
#define QDISC_FQ          4
#define QDISC_RED         5
#define QDISC_CAKE        6

/* Maximum number of qdiscs */
#define QDISC_MAX 16

/* Per-packet metadata for CoDel */
struct pkt_meta {
    uint64_t enqueue_tick;
    uint64_t dequeue_tick;
};

/* Qdisc operations */
struct qdisc {
    int type;
    void *priv;
    int (*enqueue)(struct qdisc *q, void *pkt, int len);
    void *(*dequeue)(struct qdisc *q);
    int (*drop)(struct qdisc *q);
};

/* ── API ────────────────────────────────────────────────────────── */

/* Create and manage qdiscs */
int  tc_add_qdisc(const char *dev, int qdisc_type, void *params);
int  tc_del_qdisc(const char *dev);
struct qdisc *tc_get_qdisc(const char *dev);

/* pfifo_fast qdisc */
struct qdisc *pfifo_fast_create(void);

/* fq_codel qdisc */
struct qdisc *fq_codel_create(void);

/* ── HTB (Hierarchical Token Bucket) ──────────────────────────── */

/* HTB class configuration passed to htb_add_class() */
struct htb_class_spec {
    int     parent;       /* parent class index (-1 = root) */
    uint8_t prio;         /* priority: 0 = highest, 7 = lowest */
    uint32_t rate;        /* guaranteed rate (bytes per second) */
    uint32_t ceil;        /* max rate (bytes per second) */
    uint32_t burst;       /* max burst (bytes) — 0 = auto from rate */
    uint32_t cburst;      /* ceil burst — 0 = auto from ceil */
    int     quantum;      /* bytes to serve in one round — 0 = auto */
};

/* Maximum number of HTB classes per qdisc */
#define HTB_MAX_CLASSES 32

/* Create an HTB qdisc with @num_classes slots */
struct qdisc *htb_create(void);

/* Add a class to an existing HTB qdisc.  Returns class ID (≥ 0) or -1. */
int htb_add_class(struct qdisc *q, const struct htb_class_spec *spec);

/* Remove a class by class ID.  Returns 0 on success, -1 if not found or busy. */
int htb_del_class(struct qdisc *q, int class_id);

/* Set the default class for unclassified packets */
void htb_set_default_class(struct qdisc *q, int class_id);

/* ── TBF (Token Bucket Filter) ─────────────────────────────────── */

/* TBF configuration */
struct tbf_spec {
    uint32_t rate;        /* sustained rate (bytes per second) */
    uint32_t burst;       /* max burst (bytes) — 0 = auto */
    uint32_t limit;       /* max queue depth (packets) — 0 = default 128 */
    uint32_t mtu;         /* minimum packet unit — 0 = auto (64) */
} __attribute__((packed));

/* Create a TBF qdisc with the given spec (NULL = defaults) */
struct qdisc *tbf_create(const struct tbf_spec *spec);

/* ── RED (Random Early Detection) ─────────────────────────────── */

/* RED configuration */
struct red_spec {
    uint32_t min_th;       /* minimum threshold (packets, 0 = auto) */
    uint32_t max_th;       /* maximum threshold (packets, 0 = auto) */
    uint32_t max_p;        /* maximum dropping probability (1/red_maxp_div) */
    uint32_t wq_div;       /* EWMA weight denominator (1/wq_div) — 0 = auto */
    uint32_t limit;        /* max queue depth (packets, 0 = default 256) */
    int      ecn;          /* non-zero to enable ECN marking */
};

/* Create a RED qdisc with the given spec (NULL = defaults) */
struct qdisc *red_create(const struct red_spec *spec);

/* ── FQ (Fair Queue) ────────────────────────────────────────────── */

/* FQ pacing configuration */
struct fq_pacing_spec {
    uint32_t pacing_rate;     /* bytes per second (0 = unlimited) */
    uint32_t max_credit;      /* max credit accumulation (bytes, 0 = default) */
};

/* Create an FQ qdisc with per-flow Deficit Round Robin */
struct qdisc *fq_create(void);

/* Set per-flow pacing rate.  @bucket is the flow index (0..255).
 * If @rate_bps is 0, pacing is disabled for that flow.
 * Returns 0 on success, -EINVAL on invalid bucket. */
int fq_set_pacing_rate(struct qdisc *q, int bucket, uint32_t rate_bps);

/* Set the default pacing rate applied to all new flows.
 * If @rate_bps is 0, pacing defaults to unlimited. */
void fq_set_default_pacing_rate(struct qdisc *q, uint32_t rate_bps);

/* Enable or disable pacing globally.  When disabled (0), the DRR
 * scheduler runs without rate checks, preserving the previous
 * per-flow rates for when pacing is re-enabled. */
void fq_set_pacing_enabled(struct qdisc *q, int enabled);

/* Return pacing statistics: number of times dequeue returned NULL
 * because all active flows were credit-starved (pacing-induced idle). */
void fq_get_pacing_stats(struct qdisc *q, uint64_t *pacing_idles);

/* Init */
void pkt_sched_init(void);

/* ── CAKE (Common Applications Kept Enhanced) ──────────────────── */

/* CAKE configuration */
struct cake_spec {
    uint32_t bandwidth;    /* total bandwidth (bytes/sec, 0 = default 100 Mbps) */
    uint32_t limit;        /* per-flow queue depth (packets, 0 = default 64) */
    int      ecn;          /* non-zero to enable ECN marking */
};

struct cake_stats {
    uint32_t total_bandwidth;
    int      limit;
    int      ecn_enabled;
    uint64_t tin_drops[8];
    uint64_t tin_marks[8];
    uint64_t tin_dequeued[8];
    int      tin_flows[8];
    int      tin_qlen[8];
};

/* Create a CAKE qdisc with the given spec (NULL = defaults) */
struct qdisc *cake_create(const struct cake_spec *spec);

/* ── Qdisc enumeration (for netlink interface) ──────────────────── */

/* Return the number of currently registered qdiscs */
int tc_get_qdisc_count(void);

/* Get qdisc by index (0-based). Returns NULL if out of range.
 * If name_buf is non-NULL and name_len > 0, copies the device name. */
struct qdisc *tc_get_qdisc_by_index(int i, char *name_buf, int name_len);

#endif /* PKT_SCHED_H */
