/* pkt_sched.c — Packet scheduler (qdisc) framework */

#define KERNEL_INTERNAL
#include "pkt_sched.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"

/* ── Qdisc registry ─────────────────────────────────────────────── */

static struct qdisc *qdisc_table[QDISC_MAX];
static char qdisc_names[QDISC_MAX][32];
static int qdisc_count = 0;

/* ── pfifo_fast implementation ──────────────────────────────────── */

#define PFIFO_MAX_BANDS 3
#define PFIFO_BAND_LIMIT 256

struct pfifo_fast_priv {
    void *bands[PFIFO_MAX_BANDS][PFIFO_BAND_LIMIT];
    int   band_len[PFIFO_MAX_BANDS];
    int   band_head[PFIFO_MAX_BANDS];
    int   band_tail[PFIFO_MAX_BANDS];
};

static int pfifo_fast_classify(const void *pkt, int len) {
    (void)pkt;
    (void)len;
    /* Simple classification:
     * Band 0 (highest priority): ICMP (protocol 1)
     * Band 1: TCP (protocol 6)
     * Band 2: everything else (bulk)
     * For simplicity, check first byte of IP protocol field.
     */
    const uint8_t *ip = (const uint8_t *)pkt;
    if (len >= 23) {
        uint8_t proto = ip[23]; /* IP protocol byte (assuming no options) */
        if (proto == 1) return 0;   /* ICMP → band 0 */
        if (proto == 6) return 1;   /* TCP  → band 1 */
    }
    return 2; /* bulk */
}

static int pfifo_fast_enqueue(struct qdisc *q, void *pkt, int len) {
    (void)len;
    struct pfifo_fast_priv *priv = (struct pfifo_fast_priv *)q->priv;
    int band = pfifo_fast_classify(pkt, len);

    if (priv->band_len[band] >= PFIFO_BAND_LIMIT)
        return -1;  /* queue full */

    priv->bands[band][priv->band_tail[band]] = pkt;
    priv->band_tail[band] = (priv->band_tail[band] + 1) % PFIFO_BAND_LIMIT;
    priv->band_len[band]++;
    return 0;
}

static void *pfifo_fast_dequeue(struct qdisc *q) {
    struct pfifo_fast_priv *priv = (struct pfifo_fast_priv *)q->priv;

    /* Service bands in priority order */
    for (int b = 0; b < PFIFO_MAX_BANDS; b++) {
        if (priv->band_len[b] > 0) {
            void *pkt = priv->bands[b][priv->band_head[b]];
            priv->band_head[b] = (priv->band_head[b] + 1) % PFIFO_BAND_LIMIT;
            priv->band_len[b]--;
            return pkt;
        }
    }
    return NULL;
}

static int pfifo_fast_drop(struct qdisc *q) {
    struct pfifo_fast_priv *priv = (struct pfifo_fast_priv *)q->priv;

    /* Drop from lowest priority band first */
    for (int b = PFIFO_MAX_BANDS - 1; b >= 0; b--) {
        if (priv->band_len[b] > 0) {
            priv->band_head[b] = (priv->band_head[b] + 1) % PFIFO_BAND_LIMIT;
            priv->band_len[b]--;
            return 0;
        }
    }
    return -1;
}

struct qdisc *pfifo_fast_create(void) {
    struct qdisc *q = (struct qdisc *)kmalloc(sizeof(struct qdisc));
    if (!q) return NULL;

    struct pfifo_fast_priv *priv = (struct pfifo_fast_priv *)
        kmalloc(sizeof(struct pfifo_fast_priv));
    if (!priv) {
        kfree(q);
        return NULL;
    }

    memset(priv, 0, sizeof(struct pfifo_fast_priv));
    q->type    = QDISC_PFIFO_FAST;
    q->priv    = priv;
    q->enqueue = pfifo_fast_enqueue;
    q->dequeue = pfifo_fast_dequeue;
    q->drop    = pfifo_fast_drop;
    return q;
}

/* ── fq_codel implementation ────────────────────────────────────── */

#define FQ_CODEL_QUEUES  8
#define FQ_CODEL_LIMIT   256
#define FQ_CODEL_TARGET  5     /* 5ms target */
#define FQ_CODEL_INTERVAL 100  /* 100ms interval */
#define FQ_CODEL_MTU     1500

struct codel_flow {
    void *queue[FQ_CODEL_LIMIT];
    int   head, tail, count;
    uint64_t last_drop_tick;
    int   dropping;
    uint64_t rec_inv_sqrt;
    uint64_t first_above_time;
    int   dropped;
};

struct fq_codel_priv {
    struct codel_flow flows[FQ_CODEL_QUEUES];
    int   quantum;
};

static int fq_codel_hash(const void *pkt, int len) {
    (void)pkt;
    (void)len;
    /* Simple hash based on packet address */
    return ((uintptr_t)pkt >> 4) % FQ_CODEL_QUEUES;
}

static int fq_codel_should_drop(struct codel_flow *flow, uint64_t now) {
    if (flow->count == 0) return 0;

    /* Get sojourn time: time since enqueue of oldest packet */
    void *oldest = flow->queue[flow->head];
    if (!oldest) return 0;

    struct pkt_meta *meta = (struct pkt_meta *)oldest;
    uint64_t sojourn = now - meta->enqueue_tick;

    if (sojourn < FQ_CODEL_TARGET) {
        flow->first_above_time = 0;
        return 0;
    }

    if (flow->first_above_time == 0) {
        flow->first_above_time = now;
        return 0;
    }

    if (now - flow->first_above_time >= FQ_CODEL_INTERVAL) {
        return 1;
    }
    return 0;
}

static int fq_codel_enqueue(struct qdisc *q, void *pkt, int len) {
    struct fq_codel_priv *priv = (struct fq_codel_priv *)q->priv;
    int bucket = fq_codel_hash(pkt, len);
    struct codel_flow *flow = &priv->flows[bucket];

    if (flow->count >= FQ_CODEL_LIMIT) {
        /* Drop from this queue */
        flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
        flow->count--;
    }

    flow->queue[flow->tail] = pkt;
    flow->tail = (flow->tail + 1) % FQ_CODEL_LIMIT;
    flow->count++;
    return 0;
}

static void *fq_codel_dequeue(struct qdisc *q) {
    struct fq_codel_priv *priv = (struct fq_codel_priv *)q->priv;
    uint64_t now = timer_get_ticks();

    /* Round-robin across flows */
    static int rr = 0;
    for (int i = 0; i < FQ_CODEL_QUEUES; i++) {
        int b = (rr + i) % FQ_CODEL_QUEUES;
        struct codel_flow *flow = &priv->flows[b];
        if (flow->count == 0) continue;

        if (fq_codel_should_drop(flow, now)) {
            /* Drop this packet */
            flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
            flow->count--;
            flow->dropped++;
            continue;
        }

        void *pkt = flow->queue[flow->head];
        flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
        flow->count--;
        rr = (b + 1) % FQ_CODEL_QUEUES;
        return pkt;
    }
    return NULL;
}

static int fq_codel_drop(struct qdisc *q) {
    struct fq_codel_priv *priv = (struct fq_codel_priv *)q->priv;
    for (int i = 0; i < FQ_CODEL_QUEUES; i++) {
        if (priv->flows[i].count > 0) {
            priv->flows[i].head = (priv->flows[i].head + 1) % FQ_CODEL_LIMIT;
            priv->flows[i].count--;
            return 0;
        }
    }
    return -1;
}

struct qdisc *fq_codel_create(void) {
    struct qdisc *q = (struct qdisc *)kmalloc(sizeof(struct qdisc));
    if (!q) return NULL;

    struct fq_codel_priv *priv = (struct fq_codel_priv *)
        kmalloc(sizeof(struct fq_codel_priv));
    if (!priv) {
        kfree(q);
        return NULL;
    }

    memset(priv, 0, sizeof(struct fq_codel_priv));
    priv->quantum = FQ_CODEL_MTU;

    q->type    = QDISC_FQ_CODEL;
    q->priv    = priv;
    q->enqueue = fq_codel_enqueue;
    q->dequeue = fq_codel_dequeue;
    q->drop    = fq_codel_drop;
    return q;
}

/* ── Traffic control API ────────────────────────────────────────── */

int tc_add_qdisc(const char *dev, int qdisc_type, void *params) {
    (void)params;
    if (!dev) return -1;
    if (qdisc_count >= QDISC_MAX) return -1;

    struct qdisc *q = NULL;
    switch (qdisc_type) {
        case QDISC_PFIFO_FAST:
            q = pfifo_fast_create();
            break;
        case QDISC_FQ_CODEL:
            q = fq_codel_create();
            break;
        default:
            return -1;
    }
    if (!q) return -1;

    strncpy(qdisc_names[qdisc_count], dev, 31);
    qdisc_names[qdisc_count][31] = '\0';
    qdisc_table[qdisc_count] = q;
    qdisc_count++;
    return 0;
}

int tc_del_qdisc(const char *dev) {
    if (!dev) return -1;
    for (int i = 0; i < qdisc_count; i++) {
        if (strcmp(qdisc_names[i], dev) == 0) {
            if (qdisc_table[i]->priv)
                kfree(qdisc_table[i]->priv);
            kfree(qdisc_table[i]);
            for (int j = i; j < qdisc_count - 1; j++) {
                qdisc_table[j] = qdisc_table[j + 1];
                strncpy(qdisc_names[j], qdisc_names[j + 1], 31);
            }
            qdisc_count--;
            return 0;
        }
    }
    return -1;
}

struct qdisc *tc_get_qdisc(const char *dev) {
    if (!dev) return NULL;
    for (int i = 0; i < qdisc_count; i++) {
        if (strcmp(qdisc_names[i], dev) == 0)
            return qdisc_table[i];
    }
    return NULL;
}

void pkt_sched_init(void) {
    memset(qdisc_table, 0, sizeof(qdisc_table));
    memset(qdisc_names, 0, sizeof(qdisc_names));
    qdisc_count = 0;
    kprintf("[OK] Packet scheduler initialized\\n");
}
