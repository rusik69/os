#ifndef TCP_NEWRENO_H
#define TCP_NEWRENO_H

#include "types.h"

/* Forward declaration — struct tcp_sack_block is defined in
 * net_internal.h.  Since all our SACK API functions take pointers
 * to it, a forward decl suffices to break the circular dependency
 * (net_internal.h includes tcp_newreno.h). */
struct tcp_sack_block;

/* ── SACK scoreboard (RFC 6675) ────────────────────────────────────── */

/* Threshold for scoring a segment as lost via SACK scoring.
 * If this many distinct SACK blocks cover data strictly above an
 * un-SACKed hole, that hole is considered lost even without 3 dupacks. */
#define NR_SACK_SCORE_THRESHOLD  3

struct newreno_scoreboard {
	/* Highest sequence number reported as received by SACK.
	 * Updated whenever a SACK block carries a right edge above
	 * the previous high_sacked.  Used to determine if SACK
	 * evidence exists above a candidate hole for scoring. */
	uint32_t high_sacked;

	/* Estimated bytes in flight during recovery (RFC 6675 §5).
	 * Computed as: (total outstanding) - (SACKed bytes within
	 * the outstanding range) + (retransmitted but not yet ACKed).
	 * Prevents over-sending when many segments are SACKed. */
	uint32_t pipe;

	/* Number of SACK blocks received during the current recovery
	 * episode.  Reset on recovery entry, incremented per SACK option
	 * that carries new information. */
	uint32_t sack_count;

	/* 1 after the scoreboard has been initialized for the current
	 * recovery episode.  Reset when recovery exits. */
	uint8_t  initialized;
};

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

	/* ── SACK scoreboard for enhanced loss detection (RFC 6675) ────
	 * Tracks SACK state across the current recovery episode, enabling
	 * accurate pipe estimation and loss detection below the dupack
	 * threshold.  Reset on recovery entry. */
	struct newreno_scoreboard scoreboard;
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
 * Also initialises the SACK scoreboard for the coming recovery episode
 * if SACK-capable (RFC 6675 §4).
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
 *   - Clear scoreboard
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
 * Also clears the SACK scoreboard.
 *
 * @nr      Per-connection NewReno state
 */
void newreno_abort_recovery(struct newreno_data *nr);

/* ── SACK scoreboard API (RFC 6675) ──────────────────────────────────── */

/*
 * Initialise (or re-initialise) the SACK scoreboard at the start of a
 * fast recovery episode.  Records the current outstanding range and
 * clears scoring state.
 *
 * @nr         Per-connection NewReno state
 * @last_ack   Current cumulative ACK (snd_una)
 * @our_seq    Current local sequence number (snd_nxt, i.e. highest sent)
 */
void newreno_sack_scoreboard_init(struct newreno_data *nr,
                                   uint32_t last_ack,
                                   uint32_t our_seq);

/*
 * Update the SACK scoreboard from newly-received SACK blocks.
 * Called after SACK option parsing in the TCP input path.
 * Updates high_sacked, sack_count, and pipe counters.
 *
 * @nr               Per-connection NewReno state
 * @blocks           Array of SACK blocks just received
 * @num_blocks       Number of valid SACK blocks in the array
 * @last_ack         Current cumulative ACK (snd_una)
 * @tx_unacked_len   Outstanding unacknowledged bytes
 * @tx_unacked_seq   Sequence number of first unacknowledged byte
 */
void newreno_sack_scoreboard_update(struct newreno_data *nr,
                                     const struct tcp_sack_block *blocks,
                                     int num_blocks,
                                     uint32_t last_ack,
                                     uint32_t tx_unacked_len,
                                     uint32_t tx_unacked_seq);

/*
 * Check whether a segment beginning at the given sequence number is
 * considered "lost" based on SACK scoring (RFC 6675 §4).
 *
 * A segment is lost if:
 *   1. Its sequence number falls below the cumulative ACK (snd_una), OR
 *   2. The SACK scoreboard's high_sacked is above the segment start AND
 *      at least NR_SACK_SCORE_THRESHOLD distinct SACK blocks have their
 *      right edge above the segment start.
 *
 * @nr           Per-connection NewReno state (for high_sacked)
 * @seq_start    Start sequence number of the candidate segment
 * @blocks       Current SACK blocks (may be NULL/empty)
 * @num_blocks   Number of valid blocks
 * @last_ack     Current cumulative ACK (snd_una)
 * @return       1 if the segment is scored as lost, 0 otherwise
 */
int  newreno_sack_is_lost(const struct newreno_data *nr,
                           uint32_t seq_start,
                           const struct tcp_sack_block *blocks,
                           int num_blocks,
                           uint32_t last_ack);

/*
 * Use the SACK scoreboard to find the first un-SACKed hole in the
 * outstanding data range.  Returns the byte offset from tx_unacked_seq
 * where the first missing (lost or un-SACKed) segment begins, or -1
 * if all outstanding data is SACKed (nothing to retransmit).
 *
 * This is more accurate than the simple linear skip in the main TCP
 * path because it uses the scoreboard's high_sacked and scoring to
 * detect silent losses that the basic dupack count misses.
 *
 * @nr               Per-connection NewReno state (for scoring)
 * @blocks           Current SACK blocks
 * @num_blocks       Number of valid blocks
 * @last_ack         Current cumulative ACK
 * @tx_unacked_seq   Sequence number of first unacknowledged byte
 * @tx_unacked_len   Length of outstanding data in bytes
 * @mss              Maximum segment size (typically 1400 or 1460)
 * @return           Byte offset into unacked range, or -1 if none
 */
int  newreno_sack_find_next_retransmit(const struct newreno_data *nr,
                                        const struct tcp_sack_block *blocks,
                                        int num_blocks,
                                        uint32_t last_ack,
                                        uint32_t tx_unacked_seq,
                                        uint16_t tx_unacked_len,
                                        uint16_t mss);

/*
 * Estimate the number of bytes truly in flight using the SACK scoreboard
 * (RFC 6675 §5 "Pipe Estimation").
 *
 * pipe = total_outstanding - sack_covered_bytes + retransmitted_bytes
 *
 * Where sack_covered_bytes counts the number of bytes within the current
 * outstanding range that are covered by SACK blocks.
 *
 * @last_ack         Current cumulative ACK
 * @our_seq          Current local sequence number (snd_nxt)
 * @blocks           Current SACK blocks
 * @num_blocks       Number of valid blocks
 * @tx_unacked_len   Outstanding bytes
 * @tx_unacked_seq   First unacknowledged sequence number
 * @retrans_out      Bytes retransmitted but not yet ACKed (0 if unknown)
 * @return           Estimated pipe (minimum 0, maximum total outstanding)
 */
uint32_t newreno_sack_pipe_estimate(uint32_t last_ack,
                                     uint32_t our_seq,
                                     const struct tcp_sack_block *blocks,
                                     int num_blocks,
                                     uint32_t tx_unacked_len,
                                     uint32_t tx_unacked_seq,
                                     uint32_t retrans_out);

#endif /* TCP_NEWRENO_H */
