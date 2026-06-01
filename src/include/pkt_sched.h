#ifndef PKT_SCHED_H
#define PKT_SCHED_H

#include "types.h"

/* Qdisc types */
#define QDISC_PFIFO_FAST  0
#define QDISC_FQ_CODEL    1

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

/* Init */
void pkt_sched_init(void);

#endif /* PKT_SCHED_H */
