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

#define FQ_CODEL_QUEUES      256   /* number of flow buckets */
#define FQ_CODEL_LIMIT       1024  /* per-queue limit (packets) */
#define FQ_CODEL_QUANTUM     300   /* DRR quantum (bytes) */
#define FQ_CODEL_TARGET_MS   5     /* 5ms target */
#define FQ_CODEL_INTERVAL_MS 100   /* 100ms interval */
#define FQ_CODEL_ECN_THRESH  3     /* ECN threshold (ms) — mark before dropping */

/* CoDel control law constants (fixed-point, 10 fractional bits) */
#define CODEL_INTERVAL_SHIFT   10
#define CODEL_COUNT_SHIFT      3   /* sqrt(1/count) approximation shift */

struct codel_flow {
    void *queue[FQ_CODEL_LIMIT];
    int   head, tail, count;
    uint64_t first_above_time;
    int   dropping;
    uint64_t drop_next;
    int   dropped;
    int   ecn_marked;
};

struct fq_codel_priv {
    struct codel_flow flows[FQ_CODEL_QUEUES];
    int   quantum;
    int   new_flows;       /* flow index for DRR new list (used in dequeue) */
};

/* ── Jenkins One-At-A-Time hash for 5-tuple flow hashing ────────────── */
static uint32_t jenkins_hash(const uint8_t *key, int len) {
    uint32_t hash = 0;
    for (int i = 0; i < len; i++) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

/* Parse Ethernet + IP + TCP/UDP headers to extract a 5-tuple for
 * per-flow hashing.  The incoming buffer is a raw Ethernet frame.
 *
 * Returns a flow hash (0..FQ_CODEL_QUEUES-1).
 */
static int fq_codel_flow_hash(const void *pkt, int len) {
    const uint8_t *buf = (const uint8_t *)pkt;
    uint8_t tuple[13]; /* 4+4+1+2+2 = 13 bytes for 5-tuple */
    int ti = 0;

    /* Need at least Ethernet header (14) + IP header (20) = 34 bytes */
    if (len < 34)
        return 0;

    /* Skip Ethernet header (14 bytes).  Assume no VLAN tag for simplicity. */
    const uint8_t *ip = buf + 14;
    int ip_hdr_len;

    /* Check for 802.1Q VLAN tag (EtherType 0x8100) */
    uint16_t ethertype = (uint16_t)((buf[12] << 8) | buf[13]);
    if (ethertype == 0x8100 && len >= 18) {
        /* VLAN tag present: 4 extra bytes before IP header */
        ip = buf + 18;
        if (len < 38) return 0;
    }

    /* IP version must be 4 or 6 */
    uint8_t ip_version = (ip[0] >> 4) & 0x0F;

    if (ip_version == 4) {
        /* IPv4: IHL field gives header length in 32-bit words */
        ip_hdr_len = (ip[0] & 0x0F) * 4;
        if (ip_hdr_len < 20 || ip_hdr_len > 60) return 0;
        if (len < (int)((ip - buf) + ip_hdr_len + 4)) return 0;

        uint8_t proto = ip[9];

        /* src IP (4 bytes) */
        for (int i = 0; i < 4; i++) tuple[ti++] = ip[12 + i];
        /* dst IP (4 bytes) */
        for (int i = 0; i < 4; i++) tuple[ti++] = ip[16 + i];
        /* protocol (1 byte) */
        tuple[ti++] = proto;

        /* src/dst ports for TCP/UDP */
        if ((proto == 6 || proto == 17) && (len >= (int)((ip - buf) + ip_hdr_len + 4))) {
            const uint8_t *l4 = ip + ip_hdr_len;
            tuple[ti++] = l4[0]; tuple[ti++] = l4[1]; /* src port */
            tuple[ti++] = l4[2]; tuple[ti++] = l4[3]; /* dst port */
        } else {
            tuple[ti++] = 0; tuple[ti++] = 0;
            tuple[ti++] = 0; tuple[ti++] = 0;
        }
    } else if (ip_version == 6) {
        /* IPv6 */
        if (len < (int)((ip - buf) + 40)) return 0;
        uint8_t proto = ip[6];

        /* src IP (16 bytes) — hash first 4 + last 4 */
        for (int i = 0; i < 4; i++) tuple[ti++] = ip[8 + i];
        for (int i = 0; i < 4; i++) tuple[ti++] = ip[24 + i];
        tuple[ti++] = proto;

        if ((proto == 6 || proto == 17) && (len >= (int)((ip - buf) + 44))) {
            const uint8_t *l4 = ip + 40;
            tuple[ti++] = l4[0]; tuple[ti++] = l4[1];
            tuple[ti++] = l4[2]; tuple[ti++] = l4[3];
        } else {
            tuple[ti++] = 0; tuple[ti++] = 0;
            tuple[ti++] = 0; tuple[ti++] = 0;
        }
    } else {
        /* Unknown L3 — fall back to address hash */
        return ((uintptr_t)pkt >> 4) % FQ_CODEL_QUEUES;
    }

    uint32_t h = jenkins_hash(tuple, ti);
    return (int)(h % FQ_CODEL_QUEUES);
}

/* CoDel control law: compute the next drop time using the IEEE/ACM
 * sqrt(p) approximation.  Returns absolute tick value for drop_next.
 *
 * The classic CoDel paper uses:
 *   next_drop = now + interval / sqrt(count)
 * where count is the number of drops in the current dropping interval.
 *
 * We use fixed-point with CODEL_INTERVAL_SHIFT fractional bits.
 */
static uint64_t codel_control_law(uint64_t now, int count, uint64_t interval_ticks) {
    /* Approximate 1/sqrt(count) using Newton's method (simple table). */
    uint32_t inv_sqrt;
    switch (count) {
        case 0:  inv_sqrt = 1024; break;  /* 1.0 */
        case 1:  inv_sqrt = 1024; break;
        case 2:  inv_sqrt = 724;  break;  /* 1/√2 ≈ 0.707 */
        case 3:  inv_sqrt = 591;  break;  /* 1/√3 ≈ 0.577 */
        case 4:  inv_sqrt = 512;  break;  /* 1/√4 = 0.5 */
        case 5:  inv_sqrt = 458;  break;  /* 1/√5 ≈ 0.447 */
        case 6:  inv_sqrt = 418;  break;
        case 7:  inv_sqrt = 387;  break;
        case 8:  inv_sqrt = 362;  break;
        case 9:  inv_sqrt = 341;  break;
        case 10: inv_sqrt = 324;  break;
        default: inv_sqrt = (1024 * 1024) / (count * 4); break; /* rough approx */
    }
    uint64_t delay = (interval_ticks * (uint64_t)inv_sqrt) >> CODEL_INTERVAL_SHIFT;
    if (delay < 1) delay = 1;
    return now + delay;
}

static int fq_codel_should_drop(struct codel_flow *flow, uint64_t now,
                                 uint64_t target_ticks, uint64_t interval_ticks) {
    if (flow->count == 0) return 0;

    void *oldest = flow->queue[flow->head];
    if (!oldest) return 0;

    struct pkt_meta *meta = (struct pkt_meta *)oldest;
    /* The sojourn time is tracked in microseconds via the enqueue tick.
     * timer_get_ticks() returns centiseconds (100 ticks/sec == 10ms/tick).
     * Convert to milliseconds: ticks * 10 = ms.
     * We track first_above_time as absolute tick values. */
    uint64_t sojourn_ms = (now - meta->enqueue_tick) * 10;

    if (sojourn_ms < target_ticks) {
        flow->first_above_time = 0;
        return 0;
    }

    if (flow->first_above_time == 0) {
        flow->first_above_time = now;
        return 0;
    }

    uint64_t sojourn_interval_ms = (now - flow->first_above_time) * 10;

    if (sojourn_interval_ms >= interval_ticks) {
        /* Time to drop (or mark).  Update drop_next for next control decision. */
        flow->drop_next = codel_control_law(now, flow->dropped + 1, interval_ticks / 10);
        return 1;
    }
    return 0;
}

/* ECN (Explicit Congestion Notification) marking — set ECT(1) or CE
 * in the IP header instead of dropping.  Only marks if the packet
 * is ECN-capable (ECT or CE already set).
 * Returns 1 if marked, 0 if not ECN-capable. */
static int fq_codel_ecn_mark(void *pkt, int len) {
    (void)len;
    uint8_t *buf = (uint8_t *)pkt;
    uint8_t *ip;
    uint16_t ethertype = (uint16_t)((buf[12] << 8) | buf[13]);

    if (ethertype == 0x8100 && len >= 18)
        ip = buf + 18;
    else
        ip = buf + 14;

    if (len < (int)((ip - buf) + 20)) return 0;

    uint8_t ip_version = (ip[0] >> 4) & 0x0F;
    if (ip_version != 4 && ip_version != 6) return 0;

    /* Check ECN field (bits 6-7 of the TOS/traffic-class byte).
     * The two bits are: 00=Non-ECT, 01=ECT(1), 10=ECT(0), 11=CE. */
    uint8_t ecn = ip[1] & 0x03;
    if (ecn == 0x00 || ecn == 0x03)
        return 0; /* Not ECN-capable, or already CE */

    /* Set CE (Congestion Experienced) codepoint */
    ip[1] |= 0x03;
    return 1;
}

static int fq_codel_enqueue(struct qdisc *q, void *pkt, int len) {
    struct fq_codel_priv *priv = (struct fq_codel_priv *)q->priv;
    int bucket = fq_codel_flow_hash(pkt, len);
    struct codel_flow *flow = &priv->flows[bucket];

    if (flow->count >= FQ_CODEL_LIMIT) {
        /* Queue full — drop the tail packet */
        flow->tail = (flow->tail - 1 + FQ_CODEL_LIMIT) % FQ_CODEL_LIMIT;
        flow->count--;
        flow->dropped++;
    }

    flow->queue[flow->tail] = pkt;
    flow->tail = (flow->tail + 1) % FQ_CODEL_LIMIT;
    flow->count++;
    return 0;
}

static void *fq_codel_dequeue(struct qdisc *q) {
    struct fq_codel_priv *priv = (struct fq_codel_priv *)q->priv;
    uint64_t now = timer_get_ticks();
    /* Target and interval in ticks.  TIMER_FREQ = 100 Hz → 1 tick = 10ms.
     * Convert ms to ticks: target_ticks = target_ms / 10. */
    uint64_t target_ticks   = FQ_CODEL_TARGET_MS / 10;
    uint64_t interval_ticks = FQ_CODEL_INTERVAL_MS / 10;
    if (target_ticks < 1) target_ticks = 1;
    if (interval_ticks < 1) interval_ticks = 1;

    /* Deficit Round Robin: scan flows starting from new_flows pointer.
     * This avoids starvation — eventually every flow with packets is served. */
    int start = priv->new_flows;
    for (int i = 0; i < FQ_CODEL_QUEUES; i++) {
        int b = (start + i) % FQ_CODEL_QUEUES;
        struct codel_flow *flow = &priv->flows[b];
        if (flow->count == 0) continue;

        /* CoDel: decide whether to drop or mark the head packet */
        if (fq_codel_should_drop(flow, now, target_ticks, interval_ticks)) {
            /* Try ECN marking first */
            void *head = flow->queue[flow->head];
            if (head && fq_codel_ecn_mark(head, 1500)) {
                flow->ecn_marked++;
                /* Advance past the marked packet (keep it, just mark CE) */
                flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
                flow->count--;
                if (flow->count == 0) continue;
                /* Check the new head for dropping */
                head = flow->queue[flow->head];
                if (!head) continue;
                if (fq_codel_should_drop(flow, now, target_ticks, interval_ticks)) {
                    /* Still need to drop */
                    flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
                    flow->count--;
                    flow->dropped++;
                    continue;
                }
            } else {
                /* Drop the head packet */
                flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
                flow->count--;
                flow->dropped++;
                continue;
            }
        }

        /* Dequeue the head packet */
        void *pkt = flow->queue[flow->head];
        flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
        flow->count--;
        priv->new_flows = (b + 1) % FQ_CODEL_QUEUES;
        return pkt;
    }
    return NULL;
}

static int fq_codel_drop(struct qdisc *q) {
    struct fq_codel_priv *priv = (struct fq_codel_priv *)q->priv;
    /* Find the longest queue and drop its head */
    int longest = 0;
    int longest_idx = 0;
    for (int i = 0; i < FQ_CODEL_QUEUES; i++) {
        if (priv->flows[i].count > longest) {
            longest = priv->flows[i].count;
            longest_idx = i;
        }
    }
    if (longest == 0) return -1;

    struct codel_flow *flow = &priv->flows[longest_idx];
    flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
    flow->count--;
    flow->dropped++;
    return 0;
}

void fq_codel_get_stats(struct qdisc *q, int *total_dropped, int *total_ecn_marked) {
    struct fq_codel_priv *priv = (struct fq_codel_priv *)q->priv;
    if (!priv) { *total_dropped = 0; *total_ecn_marked = 0; return; }
    int d = 0, e = 0;
    for (int i = 0; i < FQ_CODEL_QUEUES; i++) {
        d += priv->flows[i].dropped;
        e += priv->flows[i].ecn_marked;
    }
    *total_dropped = d;
    *total_ecn_marked = e;
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
    priv->quantum = FQ_CODEL_QUANTUM;
    priv->new_flows = 0;

    q->type    = QDISC_FQ_CODEL;
    q->priv    = priv;
    q->enqueue = fq_codel_enqueue;
    q->dequeue = fq_codel_dequeue;
    q->drop    = fq_codel_drop;
    return q;
}

/* ══════════════════════════════════════════════════════════════════
 *  HTB — Hierarchical Token Bucket
 *
 *  Classful queuing discipline that shapes traffic according to a
 *  user-defined hierarchy.  Each class has a guaranteed rate and
 *  a maximum ceil (burst) rate.  Classes can borrow bandwidth from
 *  their ancestors when they have spare tokens.
 *
 *  Key properties:
 *    - Classes form a tree (root has parent == -1)
 *    - Leaf classes hold packet queues; inner classes are purely
 *      structural (no queuing)
 *    - Priority (0–7) within a level; lower number = higher prio
 *    - Quantum-based round-robin among same-priority siblings
 *    - Token refill based on elapsed time since last operation
 * ══════════════════════════════════════════════════════════════════ */

/* HTB internal constants */
#define HTB_DEF_BURST_FACTOR   1600    /* Default burst = rate / 1600 * 1000 (≈ 600ms) */
#define HTB_DEF_QUANTUM        1500    /* Default quantum = 1 MTU */
#define HTB_QUEUE_LIMIT        256     /* Per-class queue depth */
#define HTB_PRIO_LEVELS        8       /* 0..7 priority levels */
#define HTB_AUTO_QUANTUM       0       /* Flag: auto-compute quantum */

/* HTB class — one node in the hierarchy */
struct htb_class {
    int      in_use;
    int      class_id;
    int      level;            /* 0 = leaf (has queue), >0 = inner */
    uint8_t  prio;             /* 0 = highest, 7 = lowest */

    /* Configuration */
    uint32_t rate;             /* guaranteed bytes per second */
    uint32_t ceil;             /* max bytes per second */
    uint32_t burst;            /* max token accumulation (bytes) */
    uint32_t cburst;           /* ceil token accumulation (bytes) */
    int      quantum;          /* bytes to serve in one round */

    /* Token buckets — fixed-point with 8 fractional bits */
    int64_t  tokens;           /* current rate tokens (bytes << 8) */
    int64_t  c_tokens;         /* current ceil tokens (bytes << 8) */
    uint64_t last_touched;     /* ticks of last token update */

    /* Hierarchy */
    int      parent;           /* parent class index, or -1 if root */
    int      children[HTB_MAX_CLASSES];
    int      num_children;

    /* Packet queue (leaf classes only) */
    void    *queue[HTB_QUEUE_LIMIT];
    int      head, tail, count;
};

/* HTB private state for a qdisc instance */
struct htb_priv {
    struct htb_class classes[HTB_MAX_CLASSES];
    int     num_classes;
    int     root_class;        /* index of root, -1 if not set */
    int     default_class;     /* class for unclassified pkts, -1 = drop */
};

/* ── Helpers ──────────────────────────────────────────────────── */

/* Locate a class by its class_id (the array index) */
static inline struct htb_class *htb_class_by_id(struct htb_priv *hp, int cid) {
    if (cid < 0 || cid >= HTB_MAX_CLASSES) return NULL;
    if (!hp->classes[cid].in_use) return NULL;
    return &hp->classes[cid];
}

/* Refill tokens for a single class based on elapsed time since
 * last_touched.  Called before any token consumption decision. */
static void htb_refill_class(struct htb_class *cl, uint64_t now) {
    if (now <= cl->last_touched)
        return;

    uint64_t elapsed = now - cl->last_touched;
    /* Rate: add (elapsed * rate / TIMER_FREQ) bytes worth of tokens,
     * clamped to burst.  TIMER_FREQ = 100 ticks/second from timer.h. */
    int64_t add = (int64_t)(elapsed * (uint64_t)cl->rate) / TIMER_FREQ;
    cl->tokens += add << 8;
    if (cl->tokens > (int64_t)cl->burst << 8)
        cl->tokens = (int64_t)cl->burst << 8;

    /* Ceil tokens */
    int64_t cadd = (int64_t)(elapsed * (uint64_t)cl->ceil) / TIMER_FREQ;
    cl->c_tokens += cadd << 8;
    if (cl->c_tokens > (int64_t)cl->cburst << 8)
        cl->c_tokens = (int64_t)cl->cburst << 8;

    cl->last_touched = now;
}

/* Recursively refill tokens for a class and all ancestors */
static void htb_refill_chain(struct htb_priv *hp, struct htb_class *cl, uint64_t now) {
    /* Refill all ancestors first (top-down) */
    if (cl->parent >= 0) {
        struct htb_class *parent = htb_class_by_id(hp, cl->parent);
        if (parent)
            htb_refill_chain(hp, parent, now);
    }
    htb_refill_class(cl, now);
}

/* Check whether a leaf can send a packet of @len bytes.
 * Returns 1 if allowed, 0 if throttled.  Refills tokens first. */
static int htb_can_send(struct htb_class *cl, int len, struct htb_class *root) {
    /* Check ceil first: can't exceed max rate even with borrowing */
    if (cl->c_tokens < ((int64_t)len << 8))
        return 0;  /* exceeded ceil */

    /* Check own rate tokens */
    if (cl->tokens >= ((int64_t)len << 8))
        return 1;  /* can send from own budget */

    /* Need to borrow from parent — if parent has spare rate tokens, we can borrow.
     * We check ancestors up to root by examining their tokens.
     * Note: we don't have hp here, but we only need to check ceil of ancestors,
     * which we can do bottom-up since the caller already refilled chains. */
    if (!root) return 0;

    /* If we reached root's ceil level, allow */
    if (root->c_tokens >= ((int64_t)len << 8))
        return 1;

    return 0;
}

/* Internal: borrow tokens from ancestors.  Called after commit. */
static void htb_borrow(struct htb_class *cl, int len) {
    int deficit = len;  /* bytes we need to cover */

    /* Charge own tokens first */
    if (cl->tokens >= ((int64_t)deficit << 8)) {
        cl->tokens -= (int64_t)deficit << 8;
        deficit = 0;
    } else {
        deficit -= (int)(cl->tokens >> 8);
        cl->tokens = 0;
    }

    /* Charge ceil tokens */
    if (cl->c_tokens >= ((int64_t)len << 8))
        cl->c_tokens -= (int64_t)len << 8;
    else
        cl->c_tokens = 0;

    /* deficit is consumed from ancestors but since we don't have
     * direct access to the hierarchy here, we simply allow the
     * packet through and the next refill will catch up. */
    (void)deficit;
}

/* ── HTB enqueue ───────────────────────────────────────────────── */

static int htb_enqueue(struct qdisc *q, void *pkt, int len) {
    struct htb_priv *hp = (struct htb_priv *)q->priv;
    if (!hp) return -1;

    uint64_t now = timer_get_ticks();

    /* Determine target class.  For now, always use default class. */
    int cid = hp->default_class;
    if (cid < 0) return -1;  /* no default, drop */

    struct htb_class *cl = htb_class_by_id(hp, cid);
    if (!cl || cl->level != 0) return -1;  /* must be leaf */

    /* Refill token chains */
    htb_refill_chain(hp, cl, now);

    /* Check if we can send (accounting for borrowing) */
    struct htb_class *root = htb_class_by_id(hp, hp->root_class);
    if (!htb_can_send(cl, len, root))
        return -1;  /* throttle — drop for now */

    /* Queue the packet */
    if (cl->count >= HTB_QUEUE_LIMIT)
        return -1;  /* queue full */

    cl->queue[cl->tail] = pkt;
    cl->tail = (cl->tail + 1) % HTB_QUEUE_LIMIT;
    cl->count++;
    return 0;
}

/* ── HTB dequeue ───────────────────────────────────────────────── */

/* Find the best leaf class to serve.  Uses DRR (deficit round-robin)
 * across priorities: scan priority levels 0..7, find a leaf that has
 * packets AND has tokens (own or borrowed). */
static struct htb_class *htb_select_leaf(struct htb_priv *hp, uint64_t now) {
    /* Simple approach: scan all leaf classes, group by priority,
     * serve the highest priority non-empty class with tokens. */
    for (int prio = 0; prio < HTB_PRIO_LEVELS; prio++) {
        for (int i = 0; i < HTB_MAX_CLASSES; i++) {
            struct htb_class *cl = &hp->classes[i];
            if (!cl->in_use || cl->level != 0) continue;
            if (cl->prio != prio) continue;
            if (cl->count == 0) continue;

            /* Refill and check tokens */
            htb_refill_chain(hp, cl, now);
            struct htb_class *root = htb_class_by_id(hp, hp->root_class);
            if (htb_can_send(cl, 64, root))  /* assume min packet */
                return cl;
        }
    }
    return NULL;
}

static void *htb_dequeue(struct qdisc *q) {
    struct htb_priv *hp = (struct htb_priv *)q->priv;
    if (!hp) return NULL;

    uint64_t now = timer_get_ticks();

    struct htb_class *cl = htb_select_leaf(hp, now);
    if (!cl) return NULL;

    /* Dequeue the packet */
    void *pkt = cl->queue[cl->head];
    cl->head = (cl->head + 1) % HTB_QUEUE_LIMIT;
    cl->count--;

    /* Estimate packet length from metadata */
    int len = 1500;  /* best-effort — caller should provide real len */

    /* Consume tokens and borrow from ancestors */
    htb_borrow(cl, len);

    return pkt;
}

/* ── HTB drop ──────────────────────────────────────────────────── */

static int htb_drop(struct qdisc *q) {
    struct htb_priv *hp = (struct htb_priv *)q->priv;
    if (!hp) return -1;

    /* Drop from lowest priority leaf that has packets */
    for (int prio = HTB_PRIO_LEVELS - 1; prio >= 0; prio--) {
        for (int i = 0; i < HTB_MAX_CLASSES; i++) {
            struct htb_class *cl = &hp->classes[i];
            if (!cl->in_use || cl->level != 0) continue;
            if (cl->prio != prio) continue;
            if (cl->count == 0) continue;

            cl->head = (cl->head + 1) % HTB_QUEUE_LIMIT;
            cl->count--;
            return 0;
        }
    }
    return -1;
}

/* Compute auto-burst: rate / Hz * 2  (2 seconds worth of data) */
static uint32_t htb_auto_burst(uint32_t rate, uint32_t ceil) {
    (void)ceil;
    /* Roughly rate * 2 seconds / 10 ticks/sec = rate / 5 */
    uint32_t b = rate / 5;
    if (b < 300) b = 300;     /* minimum 300 bytes */
    if (b > 64000) b = 64000; /* maximum 64 KB */
    return b;
}

/* ── HTB public API ────────────────────────────────────────────── */

struct qdisc *htb_create(void) {
    struct qdisc *q = (struct qdisc *)kmalloc(sizeof(struct qdisc));
    if (!q) return NULL;

    struct htb_priv *hp = (struct htb_priv *)kmalloc(sizeof(struct htb_priv));
    if (!hp) {
        kfree(q);
        return NULL;
    }

    memset(hp, 0, sizeof(struct htb_priv));
    hp->num_classes = 0;
    hp->root_class = -1;
    hp->default_class = -1;

    q->type    = QDISC_HTB;
    q->priv    = hp;
    q->enqueue = htb_enqueue;
    q->dequeue = htb_dequeue;
    q->drop    = htb_drop;
    return q;
}

int htb_add_class(struct qdisc *q, const struct htb_class_spec *spec) {
    struct htb_priv *hp = (struct htb_priv *)q->priv;
    if (!hp || !spec) return -1;

    /* Find a free slot */
    int cid = -1;
    for (int i = 0; i < HTB_MAX_CLASSES; i++) {
        if (!hp->classes[i].in_use) {
            cid = i;
            break;
        }
    }
    if (cid < 0) return -1;

    struct htb_class *cl = &hp->classes[cid];
    memset(cl, 0, sizeof(*cl));
    cl->in_use     = 1;
    cl->class_id   = cid;
    cl->parent     = spec->parent;
    cl->prio       = spec->prio > 7 ? 7 : spec->prio;
    cl->rate       = spec->rate ? spec->rate : 1000000;   /* default 1 Mbps */
    cl->ceil       = spec->ceil ? spec->ceil : cl->rate * 2; /* default 2x rate */
    if (spec->burst)
        cl->burst  = spec->burst;
    else
        cl->burst  = htb_auto_burst(cl->rate, cl->ceil);
    if (spec->cburst)
        cl->cburst = spec->cburst;
    else
        cl->cburst = htb_auto_burst(cl->ceil, cl->ceil);
    cl->quantum    = spec->quantum ? spec->quantum : HTB_DEF_QUANTUM;
    cl->level      = 0;  /* leaf by default; user can add children later */
    cl->last_touched = timer_get_ticks();

    /* Pre-fill tokens to allow initial burst */
    cl->tokens  = (int64_t)cl->burst << 8;
    cl->c_tokens = (int64_t)cl->cburst << 8;

    /* Link to parent */
    if (spec->parent >= 0 && spec->parent < HTB_MAX_CLASSES) {
        struct htb_class *parent = &hp->classes[spec->parent];
        if (parent->in_use) {
            /* Mark this class as non-leaf if it has a child */
            if (cl->level == 0)
                parent->level = 1;  /* becomes inner */
            if (parent->num_children < HTB_MAX_CLASSES)
                parent->children[parent->num_children++] = cid;
        } else {
            cl->parent = -1;  /* parent not found, make root */
        }
    }

    /* If no parent or parent not found, try to set as root */
    if (cl->parent < 0) {
        if (hp->root_class < 0) {
            hp->root_class = cid;
        } else {
            /* Multiple roots not supported — attach under existing root */
            cl->parent = hp->root_class;
            struct htb_class *root = &hp->classes[hp->root_class];
            if (root->num_children < HTB_MAX_CLASSES)
                root->children[root->num_children++] = cid;
        }
    }

    hp->num_classes++;
    return cid;
}

int htb_del_class(struct qdisc *q, int class_id) {
    struct htb_priv *hp = (struct htb_priv *)q->priv;
    if (!hp) return -1;

    struct htb_class *cl = htb_class_by_id(hp, class_id);
    if (!cl) return -1;

    /* Cannot delete a class that has children or packets */
    if (cl->num_children > 0 || cl->count > 0)
        return -1;

    /* Remove from parent's children list */
    if (cl->parent >= 0) {
        struct htb_class *parent = htb_class_by_id(hp, cl->parent);
        if (parent) {
            for (int i = 0; i < parent->num_children; i++) {
                if (parent->children[i] == class_id) {
                    for (int j = i; j < parent->num_children - 1; j++)
                        parent->children[j] = parent->children[j + 1];
                    parent->num_children--;
                    break;
                }
            }
        }
    }

    if (hp->default_class == class_id)
        hp->default_class = -1;
    if (hp->root_class == class_id)
        hp->root_class = -1;

    memset(cl, 0, sizeof(*cl));
    hp->num_classes--;
    return 0;
}

void htb_set_default_class(struct qdisc *q, int class_id) {
    struct htb_priv *hp = (struct htb_priv *)q->priv;
    if (!hp) return;
    if (class_id >= 0 && class_id < HTB_MAX_CLASSES && hp->classes[class_id].in_use)
        hp->default_class = class_id;
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
        case QDISC_HTB:
            q = htb_create();
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
    kprintf("[OK] Packet scheduler initialized\n");
}
#include "module.h"
module_init(pkt_sched_init);

/* ── Implement: pkt_sched_enqueue ────────────────── */
int pkt_sched_enqueue(void *skb, void *sch)
{
    (void)skb; (void)sch;
    kprintf("[pkt_sched] pkt_sched_enqueue: stub (basic)\n");
    return 0;
}
/* ── Implement: pkt_sched_dequeue ────────────────── */
void* pkt_sched_dequeue(void *sch)
{
    kprintf("[pkt_sched] pkt_sched_dequeue: stub (basic)\n");
    return NULL;
}
/* ── Implement: pkt_sched_register ────────────────── */
int pkt_sched_register(const char *name, void *ops)
{
    (void)name; (void)ops;
    kprintf("[pkt_sched] pkt_sched_register: stub (basic)\n");
    return 0;
}
