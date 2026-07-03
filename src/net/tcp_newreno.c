/* tcp_newreno.c — NewReno fast retransmit + fast recovery (RFC 6582)
 *                   SACK-based scoring (RFC 6675) */
#define KERNEL_INTERNAL
#include "tcp_newreno.h"
#include "net_internal.h"   /* struct tcp_sack_block */
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
 * SACK-based scoring (RFC 6675):
 *   - Maintain a SACK scoreboard tracking the highest SACKed byte and
 *     per-episode scoring state.
 *   - Use the scoreboard to detect segment loss earlier than the classic
 *     3-dupACK threshold when SACK blocks provide strong evidence.
 *   - Compute an accurate "pipe" estimate to prevent over-sending during
 *     recovery.
 *
 * NOTE: "MSS" throughout is the maximum segment size in segments.
 * Since all cwnd values are in segments, MSS = 1 segment.
 * For byte-oriented functions (pipe, scoring), the MSS parameter
 * defaults to 1400 bytes (typical Ethernet MSS with TCP options).
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
 *
 * Also initialises the SACK scoreboard for SACK-based scoring
 * (RFC 6675 §4).
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

	/* Initialise the SACK scoreboard for the coming recovery.
	 * The outstanding range is [last_ack, our_seq) but since we
	 * don't have last_ack here, the caller should call
	 * newreno_sack_scoreboard_init() after this with the correct
	 * last_ack.  We set the flag so init knows it's a new episode. */
	nr->scoreboard.initialized = 0;
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
 *   - Clear SACK scoreboard
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

	/* Clear SACK scoreboard state */
	memset(&nr->scoreboard, 0, sizeof(nr->scoreboard));
	return 1;
}

/* ── Abort recovery (on RTO timeout, connection close, etc.) ─────── */

void newreno_abort_recovery(struct newreno_data *nr)
{
	if (!nr)
		return;
	nr->in_recov = 0;
	nr->recover = 0;
	memset(&nr->scoreboard, 0, sizeof(nr->scoreboard));
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SACK scoreboard — RFC 6675 "Precise TCP Loss Detection and Recovery
 *  with Selective Acknowledgements"
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── Initialise the SACK scoreboard for a new recovery episode ──────
 *
 * Records the initial outstanding range and clears scoring counters.
 * Must be called once when fast recovery is entered, ideally right
 * after newreno_on_3dupacks(). */

void newreno_sack_scoreboard_init(struct newreno_data *nr,
                                   uint32_t last_ack,
                                   uint32_t our_seq)
{
	if (!nr)
		return;

	/* Clear any stale scoring state */
	memset(&nr->scoreboard, 0, sizeof(nr->scoreboard));

	/* The outstanding range starts at last_ack and runs to our_seq.
	 * high_sacked begins at last_ack — no SACK data above the
	 * cumulative ACK known at recovery entry. */
	nr->scoreboard.high_sacked = last_ack;
	nr->scoreboard.pipe = our_seq - last_ack; /* total outstanding bytes */
	nr->scoreboard.sack_count = 0;
	nr->scoreboard.initialized = 1;
}

/* ── Count how many SACK blocks have their right edge above a given
 *     sequence number.
 *
 * Used by the scoring function (IsLost from RFC 6675 §4).
 * Returns the number of distinct SACK blocks whose right edge is
 * strictly greater than 'seq'. */

static int sack_blocks_above(uint32_t seq,
                              const struct tcp_sack_block *blocks,
                              int num_blocks)
{
	int count = 0;

	for (int i = 0; i < num_blocks; i++) {
		if (blocks[i].left == 0 && blocks[i].right == 0)
			continue;
		/* A block contributes if its right edge is above the
		 * candidate sequence number, meaning we have evidence
		 * that at least one segment beyond this hole was
		 * received.  A right edge that equals seq means the
		 * *hole* itself is SACKed (edge case), which shouldn't
		 * happen in practice. */
		if ((int32_t)(blocks[i].right - seq) > 0)
			count++;
	}
	return count;
}

/* ── Update the SACK scoreboard from newly-received SACK blocks ─────
 *
 * Called after SACK option parsing in the TCP input path.
 * Updates:
 *   high_sacked   — the highest right edge across all SACK blocks
 *   sack_count    — accumulated count of SACK blocks seen this episode
 *   pipe          — estimated bytes truly in flight
 *
 * @blocks is the array of SACK blocks just received (already stored
 * in the connection's sack_blocks[]).  num_blocks is the count of
 * valid entries. */

void newreno_sack_scoreboard_update(struct newreno_data *nr,
                                     const struct tcp_sack_block *blocks,
                                     int num_blocks,
                                     uint32_t last_ack,
                                     uint32_t tx_unacked_len,
                                     uint32_t tx_unacked_seq)
{
	if (!nr || !blocks || num_blocks <= 0)
		return;

	if (!nr->scoreboard.initialized)
		return;

	/* 1. Update high_sacked: find the highest right edge.
	 *    The scoreboard remembers the *maximum* we've ever seen,
	 *    even if later SACK options carry shorter blocks. */
	uint32_t new_high = nr->scoreboard.high_sacked;

	for (int i = 0; i < num_blocks; i++) {
		if (blocks[i].left == 0 && blocks[i].right == 0)
			continue;
		/* Blocks that are entirely below last_ack are stale */
		if ((int32_t)(blocks[i].right - (int32_t)last_ack) <= 0)
			continue;
		if ((int32_t)(blocks[i].right - (int32_t)new_high) > 0)
			new_high = blocks[i].right;
	}

	if (new_high > nr->scoreboard.high_sacked)
		nr->scoreboard.high_sacked = new_high;

	/* 2. Increment sack_count for each new block that carries
	 *    useful data above last_ack. */
	for (int i = 0; i < num_blocks; i++) {
		if (blocks[i].left == 0 && blocks[i].right == 0)
			continue;
		if ((int32_t)(blocks[i].right - (int32_t)last_ack) > 0)
			nr->scoreboard.sack_count++;
	}

	/* 3. Recompute pipe estimate.
	 *    pipe = total_outstanding - SACKed_bytes_in_outstanding_range
	 *    where SACKed_bytes = sum of (right - left) for each block
	 *    that overlaps the outstanding range. */
	uint32_t sack_bytes = 0;

	for (int i = 0; i < num_blocks; i++) {
		if (blocks[i].left == 0 && blocks[i].right == 0)
			continue;

		/* Clip the block to the outstanding range */
		uint32_t clip_left = blocks[i].left;
		uint32_t clip_right = blocks[i].right;

		if ((int32_t)(clip_left - tx_unacked_seq) < 0)
			clip_left = tx_unacked_seq;
		if ((int32_t)(clip_right -
		    (tx_unacked_seq + tx_unacked_len)) > 0)
			clip_right = tx_unacked_seq + tx_unacked_len;

		if ((int32_t)(clip_right - clip_left) > 0)
			sack_bytes += clip_right - clip_left;
	}

	uint32_t total_outstanding = tx_unacked_len;
	if (sack_bytes >= total_outstanding)
		nr->scoreboard.pipe = 0;
	else
		nr->scoreboard.pipe = total_outstanding - sack_bytes;
}

/* ── Check if a segment is scored as lost (RFC 6675 §4) ─────────────
 *
 * A segment beginning at 'seq_start' is considered lost if:
 *   a) It has already been cumulatively ACKed (seq_start < last_ack), OR
 *   b) The SACK scoreboard has high_sacked > seq_start AND at least
 *      NR_SACK_SCORE_THRESHOLD distinct SACK blocks have their right
 *      edge strictly above seq_start.
 *
 * @nr           NewReno state (for high_sacked)
 * @seq_start    Start sequence number of the segment being tested
 * @blocks       Current SACK blocks
 * @num_blocks   Number of valid SACK blocks
 * @last_ack     Current cumulative ACK
 * @return       1 if lost, 0 if not (not enough evidence) */

int newreno_sack_is_lost(const struct newreno_data *nr,
                           uint32_t seq_start,
                           const struct tcp_sack_block *blocks,
                           int num_blocks,
                           uint32_t last_ack)
{
	if (!nr)
		return 0;

	/* (a) Already cumulatively ACKed: definitively lost */
	if ((int32_t)(seq_start - last_ack) < 0)
		return 1;

	/* (b) Not enough SACK evidence above this hole:
	 *     high_sacked must be above the candidate */
	if ((int32_t)(nr->scoreboard.high_sacked - seq_start) <= 0)
		return 0;

	/* Count SACK blocks that cover data strictly above seq_start.
	 * If ≥ NR_SACK_SCORE_THRESHOLD, the hole is scored lost. */
	int above = sack_blocks_above(seq_start, blocks, num_blocks);
	return (above >= NR_SACK_SCORE_THRESHOLD);
}

/* ── Find the next segment to retransmit using SACK scoring ─────────
 *
 * Scans the outstanding data range [tx_unacked_seq,
 * tx_unacked_seq + tx_unacked_len) in segment-sized steps (MSS bytes)
 * and finds the first segment that is either:
 *   - Not covered by any SACK block, AND scored as lost by
 *     newreno_sack_is_lost(), OR
 *   - The first un-SACKed segment if it is past a series of SACKed
 *     blocks (fallback — basic hole detection).
 *
 * Returns the byte offset from tx_unacked_seq to retransmit, or -1
 * if nothing needs retransmission (all data SACKed or ACKed).
 *
 * @nr               NewReno state (for scoring context)
 * @blocks           Current SACK blocks
 * @num_blocks       Number of valid blocks
 * @last_ack         Current cumulative ACK
 * @tx_unacked_seq   First unacknowledged sequence number
 * @tx_unacked_len   Outstanding data length
 * @mss              Segment size in bytes (default 1400)
 * @return           Byte offset, or -1 */

int newreno_sack_find_next_retransmit(const struct newreno_data *nr,
                                        const struct tcp_sack_block *blocks,
                                        int num_blocks,
                                        uint32_t last_ack,
                                        uint32_t tx_unacked_seq,
                                        uint16_t tx_unacked_len,
                                        uint16_t mss)
{
	if (!nr || !blocks || tx_unacked_len == 0 || mss == 0)
		return -1;

	uint32_t end = tx_unacked_seq + tx_unacked_len;
	uint32_t cursor = tx_unacked_seq;

	while ((int32_t)(cursor - end) < 0) {
		uint32_t seg_end = cursor + mss;
		if ((int32_t)(seg_end - end) > 0)
			seg_end = end;

		/* Check if this segment is covered by any SACK block */
		int is_sacked = 0;

		for (int i = 0; i < num_blocks; i++) {
			if (blocks[i].left == 0 && blocks[i].right == 0)
				continue;
			/* SACK block [left, right) covers [cursor, seg_end)
			 * if cursor >= left AND seg_end <= right */
			if ((int32_t)(cursor - blocks[i].left) >= 0 &&
			    (int32_t)(seg_end - blocks[i].right) <= 0) {
				is_sacked = 1;
				break;
			}
		}

		if (is_sacked) {
			/* Advance past the SACKed block */
			cursor = seg_end;
			continue;
		}

		/* This segment is NOT SACKed.  Check if it's scored
		 * as lost, or if it is simply the first un-SACKed hole
		 * after SACKed data.  In either case, retransmit it. */

		/* Use SACK scoring to confirm loss */
		if (newreno_sack_is_lost(nr, cursor, blocks,
					 num_blocks, last_ack)) {
			/* Found it — return the byte offset */
			return (int)(cursor - tx_unacked_seq);
		}

		/* Fallback: if we have SACK blocks above this segment
		 * and it's not SACKed, it's a candidate.  The simple
		 * hole heuristic: if any SACK block's left edge is above
		 * cursor, this is a hole worth repairing. */
		int has_sack_above = 0;

		for (int i = 0; i < num_blocks; i++) {
			if (blocks[i].left == 0 && blocks[i].right == 0)
				continue;
			if ((int32_t)(blocks[i].left - seg_end) > 0) {
				has_sack_above = 1;
				break;
			}
		}

		if (has_sack_above) {
			return (int)(cursor - tx_unacked_seq);
		}

		/* Move to the next segment */
		cursor = seg_end;
	}

	return -1;
}

/* ── Estimate the number of bytes in flight (pipe) using SACK ───────
 *
 * RFC 6675 §5 defines pipe as:
 *   pipe = (HighestSent - HighestAck) - (SACKed bytes in outstanding
 *           range) + (retransmitted but not yet ACKed)
 *
 * A simpler and widely-used approximation:
 *   pipe = tx_unacked_len - sack_covered_bytes
 *
 * Where sack_covered_bytes is computed by summing the intersection of
 * each SACK block with the outstanding range [tx_unacked_seq,
 * tx_unacked_seq + tx_unacked_len).
 *
 * @last_ack         Current cumulative ACK
 * @our_seq          Current local sequence number (snd_nxt)
 * @blocks           Current SACK blocks
 * @num_blocks       Number of valid blocks
 * @tx_unacked_len   Outstanding data length
 * @tx_unacked_seq   First unacknowledged sequence number
 * @retrans_out      Bytes retransmitted but not yet ACKed
 * @return           Estimated pipe value */

uint32_t newreno_sack_pipe_estimate(uint32_t last_ack,
                                     uint32_t our_seq,
                                     const struct tcp_sack_block *blocks,
                                     int num_blocks,
                                     uint32_t tx_unacked_len,
                                     uint32_t tx_unacked_seq,
                                     uint32_t retrans_out)
{
	(void)last_ack;
	(void)our_seq;
	uint32_t sack_bytes = 0;

	for (int i = 0; i < num_blocks; i++) {
		if (blocks[i].left == 0 && blocks[i].right == 0)
			continue;

		/* Clip to outstanding range */
		uint32_t clip_left = blocks[i].left;
		uint32_t clip_right = blocks[i].right;

		if ((int32_t)(clip_left - tx_unacked_seq) < 0)
			clip_left = tx_unacked_seq;
		if ((int32_t)(clip_right -
		    (tx_unacked_seq + tx_unacked_len)) > 0)
			clip_right = tx_unacked_seq + tx_unacked_len;

		if ((int32_t)(clip_right - clip_left) > 0)
			sack_bytes += clip_right - clip_left;
	}

	uint32_t outstanding = tx_unacked_len;
	uint32_t pipe = (outstanding > sack_bytes)
	              ? (outstanding - sack_bytes) + retrans_out
	              : retrans_out;

	return pipe;
}

/* ── Module information ──────────────────────────────────────────── */
/* tcp_newreno.c is compiled as part of the kernel core, not as a
 * loadable module, so MODULE_* macros don't apply here. */
