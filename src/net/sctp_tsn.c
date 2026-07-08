/* sctp_tsn.c — SCTP TSN Management (RFC 4960 §3.3.1, §3.3.4)
 *
 * Handles:
 *   - TSN allocation for DATA chunks
 *   - SACK generation on DATA reception
 *   - SACK processing for sent TSN tracking
 *   - Gap block management for out-of-order reception
 *
 * Gap blocks are stored as absolute TSNs internally and converted
 * to offset-based wire format only when building the SACK chunk.
 */

#include "sctp.h"
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "export.h"
#include "timer.h"

/* ── Allocate the next TSN for a DATA chunk ──────────────────────────── */
uint32_t sctp_tsn_alloc(struct sctp_assoc *a)
{
	if (!a)
		return 0;
	uint32_t tsn = a->next_tsn;
	a->next_tsn++;
	return tsn;
}

/* ── Sort gap blocks by start TSN (insertion sort, small list) ───────── */
static void gap_sort(struct sctp_assoc *a)
{
	/* Insertion sort — gap list is never more than 16 entries */
	for (uint8_t i = 1; i < a->num_gap_blocks; i++) {
		uint32_t key_start = a->gap_ack_start[i];
		uint32_t key_end   = a->gap_ack_end[i];
		int8_t j = (int8_t)i - 1;
		while (j >= 0 && a->gap_ack_start[j] > key_start) {
			a->gap_ack_start[j + 1] = a->gap_ack_start[j];
			a->gap_ack_end[j + 1]   = a->gap_ack_end[j];
			j--;
		}
		a->gap_ack_start[j + 1] = key_start;
		a->gap_ack_end[j + 1]   = key_end;
	}
}

/* ── Merge adjacent/overlapping gap blocks ──────────────────────────── */
static void gap_merge(struct sctp_assoc *a)
{
	if (a->num_gap_blocks == 0)
		return;

	gap_sort(a);

	uint8_t out = 0;
	for (uint8_t i = 1; i < a->num_gap_blocks; i++) {
		/* If gaps overlap or are adjacent (start <= current_end + 1) */
		if (a->gap_ack_start[i] <= a->gap_ack_end[out] + 1) {
			/* Extend */
			if (a->gap_ack_end[i] > a->gap_ack_end[out])
				a->gap_ack_end[out] = a->gap_ack_end[i];
		} else {
			/* Separate gap block */
			out++;
			a->gap_ack_start[out] = a->gap_ack_start[i];
			a->gap_ack_end[out]   = a->gap_ack_end[i];
		}
	}
	a->num_gap_blocks = out + 1;
}

/* ── Update gap block list when a new TSN arrives (absolute TSNs) ──── */
static void gap_add_tsn(struct sctp_assoc *a, uint32_t tsn)
{
	/* If this is the next expected TSN, advance cum_tsn_ack */
	if (tsn == a->cum_tsn_ack + 1) {
		a->cum_tsn_ack = tsn;

		/* Advance cum_tsn_ack while the first gap block is contiguous */
		while (a->num_gap_blocks > 0 &&
		       a->gap_ack_start[0] == a->cum_tsn_ack + 1) {
			/* The first gap block starts right after cum_tsn_ack,
			 * so advance cum_tsn_ack to cover it */
			a->cum_tsn_ack = a->gap_ack_end[0];
			/* Remove this block */
			for (uint8_t j = 0; j + 1 < a->num_gap_blocks; j++) {
				a->gap_ack_start[j] = a->gap_ack_start[j + 1];
				a->gap_ack_end[j]   = a->gap_ack_end[j + 1];
			}
			a->num_gap_blocks--;
		}
		return;
	}

	/* For out-of-order TSNs, add to gap blocks (if tsn > cum_tsn_ack) */
	if (tsn <= a->cum_tsn_ack)
		return; /* Stale — already cumulatively acked */

	/* Check if already recorded */
	for (uint8_t i = 0; i < a->num_gap_blocks; i++) {
		if (tsn >= a->gap_ack_start[i] && tsn <= a->gap_ack_end[i])
			return; /* Already present */
	}

	/* Add new gap block (single TSN for now) */
	if (a->num_gap_blocks >= SCTP_MAX_GAP_BLOCKS)
		return; /* Too many gaps — drop */

	a->gap_ack_start[a->num_gap_blocks] = tsn;
	a->gap_ack_end[a->num_gap_blocks]   = tsn;
	a->num_gap_blocks++;

	/* Merge overlapping/adjacent blocks */
	gap_merge(a);
}

/* ── Check if a TSN is a duplicate ───────────────────────────────────── */
static int tsn_is_dup(struct sctp_assoc *a, uint32_t tsn)
{
	if (tsn <= a->cum_tsn_ack)
		return 1; /* Already cumulatively acked */

	for (uint8_t i = 0; i < a->num_gap_blocks; i++) {
		if (tsn >= a->gap_ack_start[i] && tsn <= a->gap_ack_end[i])
			return 1; /* Already in gap blocks */
	}
	return 0;
}

/* ── Record a duplicate TSN ──────────────────────────────────────────── */
static void tsn_record_dup(struct sctp_assoc *a, uint32_t tsn)
{
	if (a->num_dup_tsns >= SCTP_MAX_DUP_TSNS)
		return;

	/* Check not already recorded */
	for (uint8_t i = 0; i < a->num_dup_tsns; i++) {
		if (a->dup_tsns[i] == tsn)
			return;
	}
	a->dup_tsns[a->num_dup_tsns++] = tsn;
}

/* ── Build and send a SACK chunk ────────────────────────────────────────
 *
 * Constructs a SCTP SACK chunk (RFC 4960 §3.3.4) from the association's
 * current gap block and duplicate TSN state, and sends it via IP.
 *
 * Gap blocks are stored as absolute TSNs internally; they are converted
 * to offset-based wire format here.
 */
int sctp_tsn_build_sack(struct sctp_assoc *a, uint32_t peer_ip,
                         uint16_t peer_port, uint32_t peer_tag,
                         uint16_t local_port)
{
	if (!a)
		return -EINVAL;

	uint16_t num_gaps = (a->num_gap_blocks > SCTP_MAX_GAP_BLOCKS) ?
	                     SCTP_MAX_GAP_BLOCKS : a->num_gap_blocks;
	uint16_t num_dups = (a->num_dup_tsns > SCTP_MAX_DUP_TSNS) ?
	                     SCTP_MAX_DUP_TSNS : a->num_dup_tsns;

	uint16_t gap_list_size = num_gaps * sizeof(struct sctp_gap_block);
	uint16_t dup_list_size = num_dups * sizeof(uint32_t);
	uint16_t sack_var_part = gap_list_size + dup_list_size;
	uint16_t total_chunk_len = sizeof(struct sctp_chunk) +
	                           sizeof(struct sctp_sack_hdr) -
	                           sizeof(struct sctp_chunk) +
	                           sack_var_part;
	uint16_t pkt_len = sizeof(struct sctp_header) + total_chunk_len;

	/* Big enough for worst case: SACK hdr + 16 gap blocks + 16 dup TSNs */
	uint8_t pkt[sizeof(struct sctp_header) + sizeof(struct sctp_sack_hdr) +
	            16 * sizeof(struct sctp_gap_block) + 16 * sizeof(uint32_t)];
	struct sctp_header *sh = (struct sctp_header *)pkt;
	memset(sh, 0, sizeof(*sh));
	sh->src_port = htons(local_port);
	sh->dst_port = htons(peer_port);
	sh->vtag = htonl(peer_tag);
	sh->checksum = 0;

	struct sctp_sack_hdr *sack = (struct sctp_sack_hdr *)(pkt + sizeof(*sh));
	sack->hdr.type   = SCTP_SACK;
	sack->hdr.flags  = 0;
	sack->hdr.length = htons(total_chunk_len);
	sack->cum_tsn_ack = htonl(a->cum_tsn_ack);
	sack->a_rwnd      = htonl(65536);
	sack->num_gap_blocks = htons(num_gaps);
	sack->num_dup_tsns   = htons(num_dups);

	/* Write gap blocks as offsets from cum_tsn_ack */
	struct sctp_gap_block *gaps = (struct sctp_gap_block *)(sack + 1);
	for (uint16_t i = 0; i < num_gaps; i++) {
		uint32_t start_offset = a->gap_ack_start[i] - a->cum_tsn_ack;
		uint32_t end_offset   = a->gap_ack_end[i]   - a->cum_tsn_ack;
		/* Clamp to uint16_t range */
		if (start_offset > 0xFFFF) start_offset = 0xFFFF;
		if (end_offset   > 0xFFFF) end_offset   = 0xFFFF;
		gaps[i].start = htons((uint16_t)start_offset);
		gaps[i].end   = htons((uint16_t)end_offset);
	}

	/* Write duplicate TSNs */
	uint32_t *dup_tsn_arr = (uint32_t *)(gaps + num_gaps);
	for (uint16_t i = 0; i < num_dups; i++)
		dup_tsn_arr[i] = htonl(a->dup_tsns[i]);

	send_ip(peer_ip, IPPROTO_SCTP, pkt, pkt_len);

	a->tx_packets++;

	return 0;
}

/* ── Reset per-stream state (used on close and init) ─────────────────── */
void sctp_stream_reset(struct sctp_stream *s)
{
	memset(s, 0, sizeof(*s));
}

/* ── Handle incoming DATA chunk (RFC 4960 §3.3.1) ──────────────────────
 *
 * Processes stream sequence numbers for ordered delivery, handles
 * unordered delivery (U bit), and reassembles fragmented messages
 * (B/E bits). Completed messages are placed in per-stream ready_buf.
 */
int sctp_tsn_rcv_data(struct sctp_assoc *a, uint32_t src_ip,
                       uint32_t peer_tag,
                       const struct sctp_data_hdr *dh,
                       uint16_t chunk_len)
{
	if (!a || !dh)
		return -EINVAL;

	uint32_t tsn = ntohl(dh->tsn);
	uint16_t stream_id = ntohs(dh->stream_id);
	uint16_t stream_seq = ntohs(dh->stream_seq);
	uint16_t data_len = (uint16_t)(chunk_len - sizeof(struct sctp_data_hdr));
	const uint8_t *data = (const uint8_t *)(dh + 1);

	/* Clamp stream ID */
	if (stream_id >= SCTP_MAX_STREAMS)
		stream_id = 0;

	struct sctp_stream *s = &a->in_streams[stream_id];

	/* Check ordering flag */
	int unordered = (dh->hdr.flags & SCTP_DATA_UNORDERED) ? 1 : 0;

	/* Check for duplicate or stale TSN */
	if (tsn_is_dup(a, tsn)) {
		tsn_record_dup(a, tsn);
		return 0;
	}

	/* For ordered delivery, validate stream sequence number */
	if (!unordered) {
		if (stream_seq != s->in_seq) {
			kprintf("sctp: stream %u seq mismatch: "
			        "expected %u got %u (tsn=%u)\n",
			        stream_id, s->in_seq, stream_seq, tsn);
			/* Still track TSN for SACK even if stream is
			 * out of order — stream-level and association-level
			 * ordering are independent in SCTP */
			gap_add_tsn(a, tsn);
			if (tsn > a->last_rcvd_tsn)
				a->last_rcvd_tsn = tsn;
			a->rx_packets++;
			sctp_tsn_build_sack(a, a->peer_ip, a->peer_port,
			                    peer_tag, a->local_port);
			return 0;
		}
	}

	/* Parse fragmentation bits (B = beginning, E = ending) */
	uint8_t b_bit = (dh->hdr.flags & SCTP_DATA_BEG) ? 1 : 0;
	uint8_t e_bit = (dh->hdr.flags & SCTP_DATA_END) ? 1 : 0;

	if (b_bit && e_bit) {
		/* Unfragmented message (B=1, E=1) — deliver immediately */
		uint16_t copy_len = data_len;
		if (copy_len > sizeof(s->ready_buf))
			copy_len = sizeof(s->ready_buf);
		memcpy(s->ready_buf, data, copy_len);
		s->ready_len = copy_len;
		s->ready_ppid = ntohl(dh->ppid);
		s->ready = 1;
		if (!unordered)
			s->in_seq++;
	} else if (b_bit) {
		/* Beginning fragment (B=1, E=0) — start reassembly */
		s->frag_active = 1;
		s->frag_len = 0;
		s->frag_tsn = tsn;
		s->frag_ppid = ntohl(dh->ppid);
		uint16_t copy_len = data_len;
		if (copy_len > sizeof(s->frag_buf))
			copy_len = sizeof(s->frag_buf);
		memcpy(s->frag_buf, data, copy_len);
		s->frag_len = copy_len;
	} else if (!b_bit && !e_bit) {
		/* Middle fragment (B=0, E=0) — continue reassembly */
		if (s->frag_active) {
			uint16_t room = sizeof(s->frag_buf) - s->frag_len;
			uint16_t copy_len = data_len;
			if (copy_len > room)
				copy_len = room;
			memcpy(s->frag_buf + s->frag_len, data, copy_len);
			s->frag_len += copy_len;
		}
	} else if (e_bit) {
		/* Ending fragment (B=0, E=1) — complete reassembly */
		if (s->frag_active) {
			uint16_t room = sizeof(s->frag_buf) - s->frag_len;
			uint16_t copy_len = data_len;
			if (copy_len > room)
				copy_len = room;
			memcpy(s->frag_buf + s->frag_len, data, copy_len);
			s->frag_len += copy_len;

			/* Deliver complete reassembled message */
			memcpy(s->ready_buf, s->frag_buf, s->frag_len);
			s->ready_len = s->frag_len;
			s->ready_ppid = s->frag_ppid;
			s->ready = 1;
			s->frag_active = 0;
			if (!unordered)
				s->in_seq++;
		} else {
			/* E=1 without prior B=1 — deliver as-is */
			uint16_t copy_len = data_len;
			if (copy_len > sizeof(s->ready_buf))
				copy_len = sizeof(s->ready_buf);
			memcpy(s->ready_buf, data, copy_len);
			s->ready_len = copy_len;
			s->ready_ppid = ntohl(dh->ppid);
			s->ready = 1;
			if (!unordered)
				s->in_seq++;
		}
	}

	/* Track TSN ordering (for SACK) */
	gap_add_tsn(a, tsn);
	if (tsn > a->last_rcvd_tsn)
		a->last_rcvd_tsn = tsn;

	a->rx_packets++;

	kprintf("sctp: DATA from " NIPQUAD_FMT " tsn=%u stream=%u "
	        "seq=%u len=%u %s\n",
	        NIPQUAD(src_ip), tsn, stream_id, stream_seq, data_len,
	        unordered ? "unordered" : "ordered");

	/* Send SACK */
	sctp_tsn_build_sack(a, a->peer_ip, a->peer_port,
	                    peer_tag, a->local_port);
	return 0;
}

/* ── Process incoming SACK chunk ────────────────────────────────────────
 *
 * Updates our cumulative TSN ack so we know which sent DATA chunks
 * have been received by the peer.
 */
int sctp_tsn_process_sack(struct sctp_assoc *a,
                           const struct sctp_sack_hdr *sh,
                           uint16_t chunk_len)
{
	if (!a || !sh)
		return -EINVAL;

	uint32_t cum_tsn = ntohl(sh->cum_tsn_ack);
	uint16_t num_gaps = ntohs(sh->num_gap_blocks);
	uint16_t num_dups = ntohs(sh->num_dup_tsns);

	/* Only advance — never regress cum_tsn_ack (SCTP must increment only) */
	if (cum_tsn > a->cum_tsn_ack)
		a->cum_tsn_ack = cum_tsn;

	/* Update peer's advertised receive window */
	a->peer_rwnd = ntohl(sh->a_rwnd);

	kprintf("sctp: SACK from peer cum=%u gaps=%u dups=%u "
	        "peer_rwnd=%u\n",
	        a->cum_tsn_ack, num_gaps, num_dups, a->peer_rwnd);

	(void)chunk_len;
	(void)num_gaps;
	(void)num_dups;
	return 0;
}

EXPORT_SYMBOL(sctp_tsn_alloc);
EXPORT_SYMBOL(sctp_tsn_rcv_data);
EXPORT_SYMBOL(sctp_tsn_build_sack);
EXPORT_SYMBOL(sctp_tsn_process_sack);
EXPORT_SYMBOL(sctp_stream_reset);

/* Companion file — no module_init (provided by sctp.c) */
