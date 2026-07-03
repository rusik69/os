#ifndef TCP_NEWRENO_H
#define TCP_NEWRENO_H

#include "types.h"

/* ── NewReno per-connection state (RFC 6582) ─────────────────────────── */

struct newreno_data {
	/* Highest sequence number sent when fast recovery was entered.
	 * A full ACK is one that advances the cumulative ACK to or past
	 * this value.  Until then, partial ACKs trigger a retransmit. */
	uint32_t recover;

	/* Whether we are currently in NewReno fast recovery.
	 * Duplicates c->in_recovery but scoped to this module. */
	uint8_t  in_recov;

	/* Number of partial ACKs received during this recovery episode.
	 * Diagnostic / debug use only. */
	uint32_t partial_acks;

	/* AIMD congestion avoidance counter.
	 * Incremented per ACK during congestion avoidance; when it reaches
	 * cwnd, we increment cwnd by 1 and reset.  This implements the
	 * standard Reno AIMD: cwnd += 1/cwnd per ACK. */
	uint32_t reno_ack_count;
};

/* ── NewReno API ─────────────────────────────────────────────────────── */

/* Initialize NewReno state */
void newreno_init(struct newreno_data *nr);

/*
 * Called on each ACK during normal operation (not in recovery).
 * Implements standard Reno AIMD congestion control:
 *   - Slow start (cwnd < ssthresh): cwnd++
 *   - Congestion avoidance (cwnd >= ssthresh): cwnd += 1/cwnd
 *
 * @nr         Per-connection NewReno state
 * @cwnd       Pointer to current cwnd (will be updated)
 * @ssthresh   Current slow-start threshold
 */
void newreno_on_ack(struct newreno_data *nr,
                     uint32_t *cwnd, uint32_t ssthresh);

/*
 * Called on 3rd duplicate ACK: enter fast retransmit + recovery.
 * Implements RFC 6582 §3.2 Step 1:
 *   ssthresh = max(cwnd/2, 2)
 *   cwnd = ssthresh + 3
 *   recover = our_seq
 *
 * @nr       Per-connection NewReno state
 * @cwnd     Pointer to current cwnd (will be updated)
 * @ssthresh Pointer to current ssthresh (will be updated)
 * @our_seq  Current local sequence number (the highest sent)
 */
void newreno_on_3dupacks(struct newreno_data *nr,
                          uint32_t *cwnd, uint32_t *ssthresh,
                          uint32_t our_seq);

/*
 * Called on each additional duplicate ACK during fast recovery.
 *
 * Per RFC 6582 §3.2 step 2: cwnd += MSS (1 segment).
 *
 * @nr      Per-connection NewReno state
 * @cwnd    Pointer to current cwnd (will be incremented)
 */
void newreno_on_dup_ack(struct newreno_data *nr, uint32_t *cwnd);

/*
 * Called on a partial ACK during fast recovery (the ACK advances
 * the cumulative ACK but does not cover the recovery point).
 *
 * Per RFC 6582 §3.2 step 3:
 *   - Retransmit the first unacknowledged segment
 *   - Deflate cwnd: cwnd = ssthresh
 *   - Stay in recovery (do NOT exit)
 *
 * @nr         Per-connection NewReno state
 * @cwnd       Pointer to current cwnd (will be deflated to ssthresh)
 * @ssthresh   The current slow-start threshold
 * @return     1 if caller should retransmit the head of the unacked queue
 */
int  newreno_on_partial_ack(struct newreno_data *nr,
                             uint32_t *cwnd, uint32_t ssthresh);

/*
 * Called on a full ACK (covers the recovery point or beyond).
 *
 * Per RFC 6582 §3.2 step 4:
 *   - cwnd = ssthresh
 *   - Exit fast recovery
 *   - Reset dupack_count
 *
 * @nr         Per-connection NewReno state
 * @cwnd       Pointer to current cwnd (will be set to ssthresh)
 * @ssthresh   The current slow-start threshold
 * @return     1 if recovery just ended (caller should clear in_recovery)
 */
int  newreno_on_full_ack(struct newreno_data *nr,
                          uint32_t *cwnd, uint32_t ssthresh);

/*
 * Abort a NewReno recovery (on RTO timeout, connection close, etc.).
 *
 * @nr      Per-connection NewReno state
 */
void newreno_abort_recovery(struct newreno_data *nr);

#endif /* TCP_NEWRENO_H */
