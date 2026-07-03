/* mptcp_sched.c — MPTCP path scheduler (RFC 8684 packet scheduling)
 *
 * Implements multiple path scheduler algorithms for MPTCP:
 *   - MPTCP_SCHED_MIN_RTT (default): select subflow with lowest smoothed RTT
 *   - MPTCP_SCHED_REDUNDANT: send on all active subflows (for testing/low-latency)
 *
 * The min-RTT scheduler tracks the smoothed round-trip time (srtt) from the
 * TCP connection's RTT estimator (Jacobson's algorithm, scaled by 8 in
 * tcp_conns[].srtt) and selects the subflow with the lowest srtt as the
 * primary path for the next data segment.  When two subflows have equal or
 * unmeasured RTTs, a round-robin fallback distributes load evenly.
 *
 * Reference: RFC 8684 §3.5 — Path Management and Data Sequencing
 */

#include "mptcp.h"
#include "net_internal.h"   /* tcp_conns[], struct tcp_conn */
#include "spinlock.h"
#include "errno.h"

/* ── Forward declarations of internal helpers ──────────────────── */

static int  mptcp_sched_min_rtt(struct mptcp_conn *mc);
static int  mptcp_sched_redundant(struct mptcp_conn *mc);
static int  mptcp_sched_rr_next(struct mptcp_conn *mc);
static int  mptcp_sched_count_active(const struct mptcp_conn *mc);

/* ── mptcp_sched_select: Select best subflow for next data segment ──
 *
 * Returns the subflow index (0..num_subflows-1) on success, or a negative
 * errno (-ENETDOWN) if no active subflow is available.
 */
int mptcp_sched_select(struct mptcp_conn *mc)
{
	if (!mc || !mc->used) {
		return -EINVAL;
	}

	if (mc->num_subflows == 0) {
		return -ENETDOWN;
	}

	/* Delegate to the configured algorithm */
	switch (mc->sched_algo) {
	case MPTCP_SCHED_REDUNDANT:
		return mptcp_sched_redundant(mc);
	case MPTCP_SCHED_MIN_RTT:
	default:
		return mptcp_sched_min_rtt(mc);
	}
}

/* ── mptcp_sched_get_rtt: Get smoothed RTT for a subflow ───────────
 *
 * Returns the srtt value from the TCP connection's RTT estimator.
 * The srtt is scaled by 8 per Jacobson's algorithm (units: ticks/8).
 * Returns 0 if the subflow index is invalid, the TCP connection is
 * not found, or no RTT measurement exists yet.
 *
 * Note: srtt is stored as int32_t to accommodate the scaling without
 * overflow during intermediate computations (srtt += delta - (srtt >> 3)).
 * A value of 0 means "not yet measured" because RTT is always > 0.
 */
int32_t mptcp_sched_get_rtt(const struct mptcp_conn *mc, int idx)
{
	if (!mc || idx < 0 || idx >= (int)mc->num_subflows) {
		return 0;
	}

	const struct mptcp_subflow *sf = &mc->subflows[idx];
	if (!sf->used) {
		return 0;
	}

	int conn_id = sf->conn_id;
	if (conn_id < 0 || conn_id >= MAX_TCP_CONNS) {
		return 0;
	}

	return tcp_conns[conn_id].srtt;
}

/* ── mptcp_sched_min_rtt: Min-RTT path selection ────────────────────
 *
 * Algorithm:
 *   1. Scan all active non-backup subflows and track the one with the
 *      lowest smoothed RTT (srtt).
 *   2. If only backup subflows are available, scan those instead.
 *   3. If multiple subflows have the same RTT, or no RTT measurements
 *      exist (srtt == 0), use round-robin to pick among the candidates.
 *   4. Fall back to round-robin across all active subflows if the
 *      tiebreaker cannot distinguish candidates.
 *
 * Returns subflow index on success, -ENETDOWN if no active subflow.
 */
static int mptcp_sched_min_rtt(struct mptcp_conn *mc)
{
	int best_idx = -1;
	int32_t best_rtt = INT32_MAX;
	int active_count = 0;
	int num_candidates = 0;
	int candidates[MPTCP_MAX_SUBFLOWS];

	if (!mc) {
		return -EINVAL;
	}

	/* Phase 1: scan non-backup subflows for lowest RTT */
	for (int i = 0; i < (int)mc->num_subflows; i++) {
		if (!mc->subflows[i].used) {
			continue;
		}
		active_count++;

		if (mc->subflows[i].backup) {
			continue;  /* prefer non-backup */
		}

		int32_t rtt = mptcp_sched_get_rtt(mc, i);
		if (rtt < best_rtt) {
			best_rtt = rtt;
			best_idx = i;
			num_candidates = 1;
			candidates[0] = i;
		} else if (rtt == best_rtt && best_idx >= 0) {
			/* Tie — add to candidate list for RR fallback */
			if (num_candidates < MPTCP_MAX_SUBFLOWS) {
				candidates[num_candidates++] = i;
			}
		}
	}

	/* Phase 2: if no non-backup subflow found, scan backup subflows */
	if (best_idx < 0) {
		best_rtt = INT32_MAX;
		for (int i = 0; i < (int)mc->num_subflows; i++) {
			if (!mc->subflows[i].used) {
				continue;
			}
			int32_t rtt = mptcp_sched_get_rtt(mc, i);
			if (rtt < best_rtt) {
				best_rtt = rtt;
				best_idx = i;
				num_candidates = 1;
				candidates[0] = i;
			} else if (rtt == best_rtt && best_idx >= 0) {
				if (num_candidates < MPTCP_MAX_SUBFLOWS) {
					candidates[num_candidates++] = i;
				}
			}
		}
	}

	/* Phase 3: no active subflow at all */
	if (best_idx < 0) {
		return -ENETDOWN;
	}

	/* Phase 4: if there's a tie (multiple candidates with same RTT)
	 * or no RTT measurement exists (best_rtt == 0), use RR fallback.
	 * Also use RR if we only have one candidate — just return it. */
	if (num_candidates > 1) {
		/* Round-robin among the tied candidates */
		int rr = mptcp_sched_rr_next(mc);

		/* If rr points to one of our candidates, use it.
		 * Otherwise, cycle through candidates from last_selected. */
		for (int i = 0; i < num_candidates; i++) {
			if (candidates[i] == rr) {
				return rr;
			}
		}

		/* last_selected not among candidates — advance through list */
		int next = mc->last_selected + 1;
		for (int i = 0; i < num_candidates; i++) {
			int candidate = candidates[(next + i) % num_candidates];
			if (mc->subflows[candidate].used) {
				mc->last_selected = (uint8_t)candidate;
				return candidate;
			}
		}
	}

	/* Single candidate — update last_selected and return */
	mc->last_selected = (uint8_t)best_idx;
	return best_idx;
}

/* ── mptcp_sched_redundant: Redundant path selection ──────────────
 *
 * Implements a redundant path scheduler that distributes outgoing data
 * across ALL active subflows in a round-robin fashion.  Each call to
 * mptcp_sched_select() with MPTCP_SCHED_REDUNDANT advances to the next
 * active non-backup subflow, ensuring data is spread across all paths
 * for redundancy.
 *
 * Algorithm:
 *   1. Start from last_selected + 1 (round-robin across indices).
 *   2. Scan for the next active non-backup subflow.
 *   3. If none found, scan for any active subflow (including backup).
 *   4. Update last_selected and return the winning subflow index.
 *   5. Return -ENETDOWN if no active subflows exist.
 *
 * This differs from min-RTT which picks the single best subflow;
 * redundant scheduling uses all paths equally for scenarios where
 * duplication or path diversity is desired (testing, failover, or
 * applications that handle their own deduplication).
 */
static int mptcp_sched_redundant(struct mptcp_conn *mc)
{
	if (!mc) {
		return -EINVAL;
	}

	/* Round-robin starting from last_selected + 1 */
	int start = (int)mc->last_selected + 1;
	int first_active = -ENETDOWN;
	int found = 0;

	/* Phase 1: scan for next non-backup subflow from start position */
	for (int i = 0; i < (int)mc->num_subflows; i++) {
		int idx = (start + i) % (int)mc->num_subflows;
		if (!mc->subflows[idx].used) {
			continue;
		}
		if (first_active < 0) {
			first_active = idx;  /* remember first active for fallback */
		}
		if (!mc->subflows[idx].backup && idx != (int)mc->last_selected && !found) {
			mc->last_selected = (uint8_t)idx;
			found = 1;
		}
	}

	/* If we found a non-backup different from last_selected, use it */
	if (found) {
		return (int)mc->last_selected;
	}

	/* Phase 2: if no non-backup found (or only one candidate), cycle
	 * through any active subflow from start position */
	if (first_active >= 0) {
		mc->last_selected = (uint8_t)first_active;
		return first_active;
	}

	return -ENETDOWN;
}

/* ── mptcp_sched_rr_next: Round-robin next subflow index ─────────
 *
 * Returns the next subflow index in a round-robin fashion across all
 * active subflows.  Skips inactive subflows.  Falls back to first
 * active subflow if none found after wrapping.
 */
static int mptcp_sched_rr_next(struct mptcp_conn *mc)
{
	if (!mc || mc->num_subflows == 0) {
		return -ENETDOWN;
	}

	int start = (int)mc->last_selected + 1;
	for (int i = 0; i < (int)mc->num_subflows; i++) {
		int idx = (start + i) % (int)mc->num_subflows;
		if (mc->subflows[idx].used) {
			return idx;
		}
	}

	/* Wrapped around — try from the beginning */
	for (int i = 0; i < (int)mc->num_subflows; i++) {
		if (mc->subflows[i].used) {
			return i;
		}
	}

	return -ENETDOWN;
}

/* ── mptcp_sched_count_active: Count active subflows ───────────────
 *
 * Returns the number of used (active) subflows in the connection.
 */
static int mptcp_sched_count_active(const struct mptcp_conn *mc)
{
	int count = 0;
	if (!mc) {
		return 0;
	}
	for (int i = 0; i < (int)mc->num_subflows; i++) {
		if (mc->subflows[i].used) {
			count++;
		}
	}
	return count;
}

/* ── mptcp_sched_set_algo: Set scheduler algorithm ─────────────────
 *
 * Sets the path scheduler algorithm for the MPTCP connection identified
 * by token.  The algorithm takes effect on the next call to
 * mptcp_sched_select().  Returns 0 on success, -EINVAL for invalid
 * token or algorithm.
 */
int mptcp_sched_set_algo(uint32_t token, int alg)
{
	/* Validate algorithm */
	if (alg != MPTCP_SCHED_MIN_RTT && alg != MPTCP_SCHED_REDUNDANT) {
		return -EINVAL;
	}

	spinlock_acquire(&mptcp_lock);
	struct mptcp_conn *mc = mptcp_find_by_token(token);
	if (!mc || !mc->used) {
		spinlock_release(&mptcp_lock);
		return -EINVAL;
	}
	mc->sched_algo = (uint8_t)alg;
	spinlock_release(&mptcp_lock);
	return 0;
}

/* ── mptcp_sched_get_algo: Get scheduler algorithm ─────────────────
 *
 * Returns the current path scheduler algorithm for the MPTCP connection,
 * or -EINVAL if the token is invalid.
 */
int mptcp_sched_get_algo(uint32_t token)
{
	spinlock_acquire(&mptcp_lock);
	struct mptcp_conn *mc = mptcp_find_by_token(token);
	if (!mc || !mc->used) {
		spinlock_release(&mptcp_lock);
		return -EINVAL;
	}
	int alg = (int)mc->sched_algo;
	spinlock_release(&mptcp_lock);
	return alg;
}
