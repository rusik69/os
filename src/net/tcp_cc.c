/* tcp_cc.c — Pluggable TCP congestion control framework
 *
 * Provides a registration-based dispatch layer for congestion control
 * algorithms.  Each algorithm registers a struct cc_ops and the TCP
 * stack dispatches through function pointers instead of hardcoded
 * switch statements.
 *
 * Built-in algorithms registered during cc_framework_init():
 *   CC_ALGO_CUBIC     0  — CUBIC (RFC 8312), default
 *   CC_ALGO_BBR       1  — BBR v1 (model-based)
 *   CC_ALGO_BBR3      2  — BBR v3 (ECN-aware)
 *   CC_ALGO_NEWRENO   3  — NewReno (RFC 6582)
 *   CC_ALGO_WESTWOOD  4  — Westwood+ (bandwidth est.)
 *   CC_ALGO_VEGAS     5  — Vegas (delay-based)
 *   CC_ALGO_HYBLA     6  — Hybla (satellite links)
 *   CC_ALGO_ILLINOIS  7  — Illinois (hybrid delay/loss)
 *   CC_ALGO_BIC       8  — BIC-TCP (binary search)
 *   CC_ALGO_BBR2      9  — BBR v2 (ECN/probe-RTT)
 */

#include "tcp_cc.h"
#include "net_internal.h"   /* struct tcp_conn (includes embedded CC state) */
#include "tcp_cubic.h"      /* cubic_init, cubic_update, cubic_on_loss */
#include "tcp_newreno.h"    /* newreno_init, newreno_on_ack, newreno_on_3dupacks */
#include "tcp_bbr.h"        /* bbr_init, bbr_on_ack, bbr_on_loss, bbr_get_cwnd */
#include "tcp_bbr3.h"       /* bbr3_init, bbr3_on_ack, bbr3_on_loss, bbr3_get_cwnd */
#include "tcp_westwood.h"   /* tcp_westwood_init, tcp_westwood_cong_avoid, ... */
#include "tcp_vegas.h"      /* tcp_vegas_init, tcp_vegas_cong_avoid, ... */
#include "tcp_hybla.h"      /* tcp_hybla_init, tcp_hybla_cong_avoid, ... */
#include "timer.h"          /* timer_get_ticks */
#include "string.h"
#include "printf.h"

/* ── Extern declarations for stub functions not declared in headers ── */
/* These are defined in tcp_illinois.c, tcp_bic.c, tcp_bbr2.c but not
 * declared in header files (they don't have their own headers yet).
 * Marked extern here so tcp_cc.c can reference them in the ops structs. */
extern int      tcp_illinois_cong_avoid(void *sk);
extern uint32_t tcp_illinois_ssthresh(void *sk);
extern int      tcp_bic_cong_avoid(void *sk);
extern uint32_t tcp_bic_ssthresh(void *sk);
extern int      tcp_bbr2_cong_avoid(void *sk);
extern uint32_t tcp_bbr2_ssthresh(void *sk);

/* ═══════════════════════════════════════════════════════════════════════
 *  Registration table
 * ═══════════════════════════════════════════════════════════════════════ */

/* Table of registered CC algorithm ops.  NULL = slot available. */
static struct cc_ops *cc_table[CC_ALGO_MAX]; /* zero-initialised = NULL */

/* ═══════════════════════════════════════════════════════════════════════
 *  Registration API
 * ═══════════════════════════════════════════════════════════════════════ */

int cc_register(uint8_t algo, const struct cc_ops *ops)
{
    if (algo >= CC_ALGO_MAX || !ops)
        return -EINVAL;

    /* Allow re-registration of the exact same ops (idempotent) */
    if (cc_table[algo] == ops)
        return 0;

    if (cc_table[algo] != NULL)
        return -EEXIST;  /* slot already taken by a different algorithm */

    cc_table[algo] = (struct cc_ops *)ops;
    return 0;
}

int cc_unregister(uint8_t algo)
{
    if (algo >= CC_ALGO_MAX)
        return -EINVAL;
    if (cc_table[algo] == NULL)
        return -ENOENT;

    cc_table[algo] = NULL;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Dispatch functions
 * ═══════════════════════════════════════════════════════════════════════ */

int cc_init_conn(uint8_t algo, void *sk)
{
    if (algo >= CC_ALGO_MAX || !cc_table[algo] || !cc_table[algo]->init)
        return 0;
    return cc_table[algo]->init(sk);
}

int cc_cong_avoid(uint8_t algo, void *sk)
{
    if (algo >= CC_ALGO_MAX || !cc_table[algo] || !cc_table[algo]->cong_avoid)
        return 0;
    return cc_table[algo]->cong_avoid(sk);
}

uint32_t cc_ssthresh(uint8_t algo, void *sk)
{
    if (algo >= CC_ALGO_MAX || !cc_table[algo] || !cc_table[algo]->ssthresh)
        return 2;  /* default: minimum safe ssthresh */
    return cc_table[algo]->ssthresh(sk);
}

int cc_acked(uint8_t algo, void *sk, uint32_t acked)
{
    if (algo >= CC_ALGO_MAX || !cc_table[algo] || !cc_table[algo]->acked)
        return 0;
    return cc_table[algo]->acked(sk, acked);
}

void cc_on_loss(uint8_t algo, void *sk)
{
    if (algo >= CC_ALGO_MAX || !cc_table[algo] || !cc_table[algo]->on_loss)
        return;
    cc_table[algo]->on_loss(sk);
}

uint32_t cc_get_cwnd(uint8_t algo, void *sk)
{
    if (algo >= CC_ALGO_MAX || !cc_table[algo] || !cc_table[algo]->get_cwnd)
        return 10;  /* default fallback */
    return cc_table[algo]->get_cwnd(sk);
}

const char *cc_name(uint8_t algo)
{
    if (algo >= CC_ALGO_MAX || !cc_table[algo] || !cc_table[algo]->name)
        return "unknown";
    return cc_table[algo]->name;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Wrapper callbacks for algorithms that use internal (embedded) API
 *
 *  These wrappers cast void *sk → struct tcp_conn * and access the
 *  embedded per-algorithm state (cubic_data, newreno_data, bbr_data,
 *  bbr3_data) through the struct tcp_conn member fields.
 *
 *  Algorithms that already have separate callback wrappers in their
 *  own .c files (Westwood, Vegas, Hybla) use those directly and don't
 *  need wrappers here.
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── CUBIC wrappers (cc_algo == 0) ───────────────────────────────── */

static int cubic_wrapper_init(void *sk)
{
    if (!sk) return -EINVAL;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    cubic_init(&conn->cubic);
    return 0;
}

static int cubic_wrapper_cong_avoid(void *sk)
{
    if (!sk) return -EINVAL;
    struct tcp_conn *conn = (struct tcp_conn *)sk;

    if (conn->cwnd < conn->ssthresh) {
        /* Slow start: exponential growth */
        uint64_t now_ms = timer_get_ms();
        uint32_t rtt_delta = (uint32_t)(timer_get_ticks() -
                                        conn->last_send_tick);
        uint32_t rtt_ms = rtt_delta * 10;
        if (cubic_hystart_update(&conn->cubic, rtt_ms, now_ms))
            conn->ssthresh = conn->cwnd;
        conn->cwnd++;
    } else {
        /* Congestion avoidance: cubic function */
        uint64_t now = timer_get_ticks();
        uint32_t rtt_ticks = (conn->srtt > 0) ?
                             (uint32_t)(conn->srtt / 8) : 10;
        if (rtt_ticks < 1) rtt_ticks = 1;
        conn->cwnd = cubic_update(&conn->cubic, conn->cwnd, now, rtt_ticks);
    }
    return 0;
}

static uint32_t cubic_wrapper_ssthresh(void *sk)
{
    if (!sk) return 2;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    return cubic_on_loss(&conn->cubic, conn->cwnd, timer_get_ticks());
}

static void cubic_wrapper_on_loss(void *sk)
{
    if (!sk) return;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    conn->ssthresh = cubic_on_loss(&conn->cubic, conn->cwnd, timer_get_ticks());
}

static const struct cc_ops cubic_cc_ops = {
    .name       = "cubic",
    .init       = cubic_wrapper_init,
    .cong_avoid = cubic_wrapper_cong_avoid,
    .ssthresh   = cubic_wrapper_ssthresh,
    .on_loss    = cubic_wrapper_on_loss,
};

/* ── NewReno wrappers (cc_algo == 3) ─────────────────────────────── */

static int newreno_wrapper_init(void *sk)
{
    if (!sk) return -EINVAL;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    newreno_init(&conn->newreno);
    return 0;
}

static int newreno_wrapper_cong_avoid(void *sk)
{
    if (!sk) return -EINVAL;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    newreno_on_ack(&conn->newreno, &conn->cwnd, conn->ssthresh);
    return 0;
}

static uint32_t newreno_wrapper_ssthresh(void *sk)
{
    if (!sk) return 2;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    /* NewReno: standard Reno half-reduction */
    uint32_t val = conn->cwnd / 2;
    return (val < 2) ? 2 : val;
}

static void newreno_wrapper_on_loss(void *sk)
{
    if (!sk) return;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    newreno_on_3dupacks(&conn->newreno, &conn->cwnd,
                        &conn->ssthresh, conn->our_seq);
}

static const struct cc_ops newreno_cc_ops = {
    .name       = "newreno",
    .init       = newreno_wrapper_init,
    .cong_avoid = newreno_wrapper_cong_avoid,
    .ssthresh   = newreno_wrapper_ssthresh,
    .on_loss    = newreno_wrapper_on_loss,
};

/* ── BBR wrappers (cc_algo == 1) ───────────────────────────────────
 *
 * BBR is model-based and its per-ACK processing (bbr_on_ack) needs
 * byte-count and RTT samples that aren't available through the simple
 * cong_avoid interface.  The wrappers below provide basic init/loss
 * dispatch; the full per-ACK processing stays in net_tcp.c for now.
 * See bbr_on_ack() in tcp_bbr.c for the model update logic.
 */

static int bbr_wrapper_init(void *sk)
{
    if (!sk) return -EINVAL;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    bbr_init(&conn->bbr);
    return 0;
}

static void bbr_wrapper_on_loss(void *sk)
{
    if (!sk) return;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    bbr_on_loss(&conn->bbr);
}

static uint32_t bbr_wrapper_get_cwnd(void *sk)
{
    if (!sk) return 10;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    return bbr_get_cwnd(&conn->bbr, conn->cwnd);
}

static const struct cc_ops bbr_cc_ops = {
    .name     = "bbr",
    .init     = bbr_wrapper_init,
    .on_loss  = bbr_wrapper_on_loss,
    .get_cwnd = bbr_wrapper_get_cwnd,
};

/* ── BBRv3 wrappers (cc_algo == 2) ────────────────────────────────── */

static int bbr3_wrapper_init(void *sk)
{
    if (!sk) return -EINVAL;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    bbr3_init(&conn->bbr3);
    return 0;
}

static void bbr3_wrapper_on_loss(void *sk)
{
    if (!sk) return;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    bbr3_on_loss(&conn->bbr3);
}

static uint32_t bbr3_wrapper_get_cwnd(void *sk)
{
    if (!sk) return 10;
    struct tcp_conn *conn = (struct tcp_conn *)sk;
    return bbr3_get_cwnd(&conn->bbr3, conn->cwnd);
}

static const struct cc_ops bbr3_cc_ops = {
    .name     = "bbr3",
    .init     = bbr3_wrapper_init,
    .on_loss  = bbr3_wrapper_on_loss,
    .get_cwnd = bbr3_wrapper_get_cwnd,
};

/* ── Westwood (cc_algo == 4) ─────────────────────────────────────────
 *  Uses existing tcp_westwood_* callbacks from tcp_westwood.c.
 *  Declared in tcp_westwood.h.  No wrappers needed here. */

static const struct cc_ops westwood_cc_ops = {
    .name       = "westwood",
    .init       = tcp_westwood_init,
    .cong_avoid = tcp_westwood_cong_avoid,
    .ssthresh   = tcp_westwood_ssthresh,
    .acked      = tcp_westwood_acked,
};

/* ── Vegas (cc_algo == 5) ─────────────────────────────────────────────
 *  Uses existing tcp_vegas_* callbacks from tcp_vegas.c. */

static const struct cc_ops vegas_cc_ops = {
    .name       = "vegas",
    .init       = tcp_vegas_init,
    .cong_avoid = tcp_vegas_cong_avoid,
    .ssthresh   = tcp_vegas_ssthresh,
    .acked      = tcp_vegas_acked,
};

/* ── Hybla (cc_algo == 6) ─────────────────────────────────────────────
 *  Uses existing tcp_hybla_* callbacks from tcp_hybla.c. */

static const struct cc_ops hybla_cc_ops = {
    .name       = "hybla",
    .init       = tcp_hybla_init,
    .cong_avoid = tcp_hybla_cong_avoid,
    .ssthresh   = tcp_hybla_ssthresh,
    .acked      = tcp_hybla_acked,
};

/* ── Illinois (cc_algo == 7) ───────────────────────────────────────────
 *  Illinois uses struct illinois_data which is NOT embedded in
 *  struct tcp_conn (it's local to tcp_illinois.c).  The existing
 *  tcp_illinois_* stubs are no-ops.  We register stub ops that
 *  provide the name and basic defaults so the algorithm can be
 *  selected without crashing, even though it won't do real CC. */

static int illinois_stub_init(void *sk)
{
    (void)sk;
    return 0;
}

static const struct cc_ops illinois_cc_ops = {
    .name       = "illinois",
    .init       = illinois_stub_init,
    .ssthresh   = tcp_illinois_ssthresh,
    .cong_avoid = tcp_illinois_cong_avoid,
};

/* ── BIC (cc_algo == 8) ────────────────────────────────────────────────
 *  Same situation as Illinois — struct bic_data is NOT embedded in
 *  struct tcp_conn.  Stub ops for selection safety. */

static int bic_stub_init(void *sk)
{
    (void)sk;
    return 0;
}

static const struct cc_ops bic_cc_ops = {
    .name       = "bic",
    .init       = bic_stub_init,
    .ssthresh   = tcp_bic_ssthresh,
    .cong_avoid = tcp_bic_cong_avoid,
};

/* ── BBRv2 (cc_algo == 9) ──────────────────────────────────────────────
 *  BBRv2 is a loadable kernel module (tcp_bbr2.ko).  Its state is NOT
 *  embedded in struct tcp_conn.  Stub ops only — the module registers
 *  its own ops at init time if loaded. */

static int bbr2_stub_init(void *sk)
{
    (void)sk;
    return 0;
}

static const struct cc_ops bbr2_cc_ops = {
    .name       = "bbr2",
    .init       = bbr2_stub_init,
    .ssthresh   = tcp_bbr2_ssthresh,
    .cong_avoid = tcp_bbr2_cong_avoid,
};

/* ═══════════════════════════════════════════════════════════════════════
 *  Framework initialisation
 * ═══════════════════════════════════════════════════════════════════════ */

void cc_framework_init(void)
{
    int ret;

    /* Zero the table (re-init safety) */
    memset(cc_table, 0, sizeof(cc_table));

    ret = cc_register(CC_ALGO_CUBIC,     &cubic_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register CUBIC: %d\n", ret);

    ret = cc_register(CC_ALGO_BBR,       &bbr_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register BBR: %d\n", ret);

    ret = cc_register(CC_ALGO_BBR3,      &bbr3_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register BBRv3: %d\n", ret);

    ret = cc_register(CC_ALGO_NEWRENO,   &newreno_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register NewReno: %d\n", ret);

    ret = cc_register(CC_ALGO_WESTWOOD,  &westwood_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register Westwood: %d\n", ret);

    ret = cc_register(CC_ALGO_VEGAS,     &vegas_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register Vegas: %d\n", ret);

    ret = cc_register(CC_ALGO_HYBLA,     &hybla_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register Hybla: %d\n", ret);

    ret = cc_register(CC_ALGO_ILLINOIS,  &illinois_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register Illinois: %d\n", ret);

    ret = cc_register(CC_ALGO_BIC,       &bic_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register BIC: %d\n", ret);

    ret = cc_register(CC_ALGO_BBR2,      &bbr2_cc_ops);
    if (ret < 0) kprintf("[CC] Failed to register BBRv2: %d\n", ret);

    kprintf("[CC] Congestion control framework initialised — "
            "%d algorithms registered\n", CC_ALGO_COUNT);
}
