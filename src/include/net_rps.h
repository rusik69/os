#ifndef NET_RPS_H
#define NET_RPS_H

/*
 * net_rps.h — Receive Packet Steering / Flow Steering (RPS/RFS)
 *
 * RPS distributes incoming packet processing across CPUs based on a
 * flow hash (IP src/dst + protocol + port), enabling parallel receive
 * processing on multi-core systems.
 *
 * RFS extends RPS by tracking which CPU an application is using for
 * each flow and steering packets to that CPU, improving cache locality.
 *
 * Architecture:
 *   - Each CPU has a per-CPU backlog queue (struct rps_backlog)
 *   - Incoming packets are hashed → target CPU → enqueued on backlog
 *   - Each CPU drains its own backlog in its idle/poll loop
 *   - RFS flow table maps (src_ip,dst_ip,src_port,dst_port,proto) → last_cpu
 */

#include "types.h"
#include "smp.h"

/* ── Constants ──────────────────────────────────────────────────────── */

/* Maximum number of RPS backlog entries per CPU */
#define RPS_BACKLOG_SIZE   64

/* Maximum number of RFS flow table entries */
#define RFS_FLOW_TABLE_SIZE 256

/* ── Flow hash (5-tuple: src_ip, dst_ip, src_port, dst_port, proto) ── */

struct rps_flow_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t  proto;       /* IP protocol (TCP=6, UDP=17) */
};

/* ── RPS: Per-CPU backlog queue ──────────────────────────────────────── */

/* Maximum packet data we store in the backlog (ethernet MTU) */
#define RPS_MTU_SIZE 2048

/* A single entry in the RPS backlog */
struct rps_backlog_entry {
    uint8_t data[RPS_MTU_SIZE];  /* Packet data */
    uint16_t len;                /* Actual packet length */
};

/* Per-CPU backlog — separate allocation, pointer in cpu_info */
struct rps_backlog {
    struct rps_backlog_entry slots[RPS_BACKLOG_SIZE];
    int  head;            /* Dequeue index (consumer) */
    int  tail;            /* Enqueue index (producer) */
    int  count;           /* Number of pending packets */
    int  cpu_id;          /* Owning CPU ID */
};

/* ── RFS: Flow-to-CPU mapping ────────────────────────────────────────── */

struct rfs_flow_entry {
    struct rps_flow_key key;
    int  last_cpu;        /* CPU that last processed this flow */
    uint64_t last_seen;   /* Timestamp (ticks) of last packet */
    uint8_t  in_use;
};

/* RFS flow table */
struct rfs_flow_table {
    struct rfs_flow_entry entries[RFS_FLOW_TABLE_SIZE];
    int count;
};

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialize RPS/RFS subsystem */
void rps_rfs_init(void);

/* Compute a hash over the 5-tuple flow key.  Returns a CPU ID. */
int rps_flow_hash(const struct rps_flow_key *key);

/* Enqueue a packet on the target CPU's RPS backlog.
 * Returns 0 on success, -1 if backlog is full.
 * The caller should process the packet directly if enqueue fails. */
int rps_enqueue(int target_cpu, const uint8_t *data, uint16_t len);

/* Dequeue and process the next packet from this CPU's RPS backlog.
 * Called from the per-CPU poll/idle loop.
 * Returns 0 if a packet was processed, -1 if backlog empty. */
int rps_process_backlog(void);

/* RFS: Look up which CPU last handled a flow.
 * Returns CPU ID, or -1 if flow not tracked. */
int rfs_lookup_cpu(const struct rps_flow_key *key);

/* RFS: Record that a flow was processed on the given CPU. */
void rfs_record_flow(const struct rps_flow_key *key, int cpu);

/* RFS: Update flow-to-CPU mapping based on where the application runs.
 * Called during socket operations to track the current process's CPU. */
void rfs_update_flow_cpu(const struct rps_flow_key *key);

/* Get pointer to this CPU's RPS backlog */
static inline struct rps_backlog *rps_this_cpu_backlog(void) {
    return get_cpu_info()->rps_backlog;
}

#endif /* NET_RPS_H */
