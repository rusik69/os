#ifndef PKT_SCHED_H
#define PKT_SCHED_H

#include "types.h"

/* Qdisc types */
#define QDISC_PFIFO_FAST  0
#define QDISC_FQ_CODEL    1
#define QDISC_HTB         2
#define QDISC_TBF         3

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

/* Init */
void pkt_sched_init(void);

#endif /* PKT_SCHED_H */
