/* tcp_newreno.c — NewReno fast retransmit + fast recovery (RFC 6582) */

#include "tcp_newreno.h"
#include "types.h"
#include "string.h"

/*
 * NewReno Fast Retransmit and Fast Recovery (RFC 6582)
 *
 * NewReno refines TCP Reno's fast recovery by handling partial ACKs
 * correctly.  In standard Reno, a partial ACK (which ACKs the retransmitted
 * segment but not all outstanding data) causes an exit from fast recovery,
 * reducing cwnd to ssthresh and potentially causing multiple window
 * reductions for a single window of losses.
 *
 * NewReno stays in recovery during partial ACKs, retransmitting one lost
 * segment per partial ACK.  This allows multiple losses within a single
 * window to be repaired without repeated window reductions.
 *
 * Algorithm (RFC 6582 §3.2):
 *
 *   1. On 3rd duplicate ACK:
 *      - ssthresh = max(FlightSize / 2, 2 * MSS)
 *      - cwnd = ssthresh + 3 * MSS
 *      - Retransmit the lost segment
 *      - Set recover = HighestSeqSent
 *      - Enter fast recovery
 *
 *   2. For each additional duplicate ACK during recovery:
 *      - cwnd += MSS (inflate window to allow new segments)
 *
 *   3. On partial ACK (ACKs new data but ACK < recover):
 *      - Retransmit the first unacknowledged segment
 *      - cwnd = ssthresh (deflate back)
 *      - Stay in recovery (do NOT exit)
 *
 *   4. On full ACK (ACK >= recover):
 *      - cwnd = ssthresh
 *      - Exit fast recovery
 *      - Reset dupack_count
 *
 * NOTE: "MSS" throughout is the maximum segment size in segments.
 * Since all cwnd values are in segments, MSS = 1 segment.
 */

/* ── Initialize NewReno state ─────────────────────────────────────── */

void newreno_init(struct newreno_data *nr)
{
	if (!nr)
		return;
	memset(nr, 0, sizeof(*nr));
}

/* ── ACK processing (normal operation, not in recovery) ──────────────
 *
 * Standard Reno AIMD congestion control:
 *   Slow start (cwnd < ssthresh):   cwnd += 1 per ACK (doubles per RTT)
 *   Congestion avoidance (cwnd >= ssthresh): cwnd += 1/cwnd per ACK
 */

void newreno_on_ack(struct newreno_data *nr,
                     uint32_t *cwnd, uint32_t ssthresh)
{
	if (!nr || !cwnd)
		return;

	if (*cwnd < ssthresh) {
		/* Slow start: exponential growth */
		(*cwnd)++;
	} else {
		/* Congestion avoidance: AIMD (cwnd += 1/cwnd per ACK).
		 * Count ACKs; increment cwnd when we've received a full
		 * window of ACKs. */
		nr->reno_ack_count++;
		while (nr->reno_ack_count >= *cwnd) {
			nr->reno_ack_count -= *cwnd;
			(*cwnd)++;
		}
	}
}

/* ── On 3rd duplicate ACK: enter fast retransmit + recovery ─────────
 *
 * Per RFC 6582 §3.2 Step 1:
 *   ssthresh = max(FlightSize / 2, 2 * MSS)
 *   cwnd = ssthresh + 3 * MSS
 *   recover = our_seq (highest sent)
 *   Enter fast recovery
 */

void newreno_on_3dupacks(struct newreno_data *nr,
                          uint32_t *cwnd, uint32_t *ssthresh,
                          uint32_t our_seq)
{
	if (!nr || !cwnd || !ssthresh)
		return;

	/* ssthresh = max(cwnd / 2, 2) — in segments */
	uint32_t new_ssthresh = *cwnd / 2;
	if (new_ssthresh < 2)
		new_ssthresh = 2;
	*ssthresh = new_ssthresh;

	/* cwnd = ssthresh + 3 * MSS (in segments) */
	*cwnd = new_ssthresh + 3;

	/* Record the recovery point — the highest sequence number sent */
	nr->recover = our_seq;
	nr->in_recov = 1;
	nr->partial_acks = 0;
}

/* ── On additional duplicate ACK during recovery ────────────────────
 *
 * RFC 6582 §3.2 Step 2:
 *   cwnd += MSS (1 segment) per additional duplicate ACK
 *   This inflates the window so we can send a new segment.
 */

void newreno_on_dup_ack(struct newreno_data *nr, uint32_t *cwnd)
{
	(void)nr;
	if (!cwnd)
		return;

	/* cwnd += 1 segment per additional dupack */
	(*cwnd)++;
}

/* ── On partial ACK during recovery ──────────────────────────────────
 *
 * RFC 6582 §3.2 Step 3:
 *   A partial ACK is one that ACKs some new data but does not reach
 *   the recovery point (ACK < recover).
 *
 *   Actions:
 *   - Retransmit the first unacknowledged segment
 *   - cwnd = ssthresh (deflate back to the reduced value)
 *   - Stay in recovery (DON'T exit)
 *
 * Returns 1 if the caller should retransmit the head of the unacked
 * queue (the first missing segment).
 */

int newreno_on_partial_ack(struct newreno_data *nr,
                            uint32_t *cwnd, uint32_t ssthresh)
{
	if (!nr || !cwnd)
		return 0;

	/* Ensure we are still in recovery */
	if (!nr->in_recov)
		return 0;

	/* Deflate: cwnd = ssthresh */
	*cwnd = ssthresh;

	/* Track partial ACKs for diagnostic purposes */
	nr->partial_acks++;

	/* Signal the caller to retransmit the first unacknowledged segment */
	return 1;
}

/* ── On full ACK: exit fast recovery ─────────────────────────────────
 *
 * RFC 6582 §3.2 Step 4:
 *   When ACK >= recover, all outstanding data has been delivered.
 *   - cwnd = ssthresh
 *   - Exit fast recovery
 *   - Reset dupack_count
 *
 * Returns 1 if recovery just ended.
 */

int newreno_on_full_ack(struct newreno_data *nr,
                         uint32_t *cwnd, uint32_t ssthresh)
{
	if (!nr || !cwnd)
		return 0;

	if (!nr->in_recov)
		return 0;

	/* Deflate: cwnd = ssthresh */
	*cwnd = ssthresh;

	/* Exit recovery */
	nr->in_recov = 0;
	return 1;
}

/* ── Abort recovery (on RTO timeout, connection close, etc.) ─────── */

void newreno_abort_recovery(struct newreno_data *nr)
{
	if (!nr)
		return;
	nr->in_recov = 0;
	nr->recover = 0;
}

/* ── Module information ──────────────────────────────────────────── */
/* tcp_newreno.c is compiled as part of the kernel core, not as a
 * loadable module, so MODULE_* macros don't apply here. */
