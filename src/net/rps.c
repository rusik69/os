/*
 * rps.c — Receive Packet Steering / Flow Steering (RPS/RFS)
 *
 * RPS distributes incoming packet processing across CPUs based on a
 * flow hash, enabling parallel receive processing on multi-core systems.
 *
 * RFS extends RPS by tracking which CPU an application is using for
 * each flow and steering packets to that CPU, improving cache locality.
 *
 * This implementation provides:
 *   - A per-CPU backlog queue for incoming packets
 *   - Flow hashing (Jenkins hash over 5-tuple)
 *   - RFS flow table to track flow→CPU mapping
 *   - Integration hooks for the network receive path
 *
 * Reference: Linux kernel RPS/RFS (net/core/dev.c)
 */

#define KERNEL_INTERNAL
#include "net_rps.h"
#include "net.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "smp.h"

/* ── RFS flow table (global, protected by simple lock) ────────────── */

static struct rfs_flow_table g_rfs_table;
static int g_rps_rfs_initialized = 0;

/* ── Internal: Jenkins one-at-a-time hash ─────────────────────────────
 *
 * A simple, fast, avalanche-aware hash suitable for flow steering.
 * Operates on the 5-tuple: src_ip(4) + dst_ip(4) + src_port(2) +
 * dst_port(2) + proto(1) = 13 bytes.
 */
static uint32_t jenkins_one_at_a_time(const uint8_t *key, int len)
{
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

/* ── Public API ───────────────────────────────────────────────────── */

void rps_rfs_init(void)
{
    if (g_rps_rfs_initialized) return;

    /* Initialize RFS flow table */
    memset(&g_rfs_table, 0, sizeof(g_rfs_table));

    /* Allocate per-CPU RPS backlogs */
    int cpu_count = smp_get_cpu_count();
    if (cpu_count < 1) cpu_count = 1;

    for (int i = 0; i < cpu_count && i < SMP_MAX_CPUS; i++) {
        struct rps_backlog *bl = (struct rps_backlog *)kmalloc(sizeof(struct rps_backlog));
        if (!bl) {
            kprintf("[rps] WARNING: failed to allocate backlog for CPU %d\n", i);
            continue;
        }
        memset(bl, 0, sizeof(struct rps_backlog));
        bl->head   = 0;
        bl->tail   = 0;
        bl->count  = 0;
        bl->cpu_id = i;

        cpu_info_array[i].rps_backlog = bl;
    }

    g_rps_rfs_initialized = 1;
    kprintf("[OK] RPS/RFS initialized: %d CPUs, %d backlog slots per CPU\n",
            cpu_count, RPS_BACKLOG_SIZE);
}

int rps_flow_hash(const struct rps_flow_key *key)
{
    if (!key) return 0;

    /* Hash the 5-tuple (13 bytes) */
    uint32_t h = jenkins_one_at_a_time((const uint8_t *)key,
        sizeof(struct rps_flow_key));

    /* Map hash to a CPU ID */
    int cpu_count = smp_get_cpu_count();
    if (cpu_count <= 1) return 0;

    return (int)(h % (uint32_t)cpu_count);
}

int rps_enqueue(int target_cpu, const uint8_t *data, uint16_t len)
{
    if (target_cpu < 0 || target_cpu >= SMP_MAX_CPUS)
        return -1;
    if (!data || len == 0 || len > RPS_MTU_SIZE)
        return -1;

    struct rps_backlog *bl = cpu_info_array[target_cpu].rps_backlog;
    if (!bl)
        return -1;

    /* Check if backlog is full */
    if (bl->count >= RPS_BACKLOG_SIZE)
        return -1;

    /* Enqueue the packet (circular buffer) */
    int slot = bl->tail;
    memcpy(bl->slots[slot].data, data, len);
    bl->slots[slot].len = len;

    bl->tail = (bl->tail + 1) % RPS_BACKLOG_SIZE;
    bl->count++;

    return 0;
}

int rps_process_backlog(void)
{
    struct rps_backlog *bl = rps_this_cpu_backlog();
    if (!bl)
        return -1;

    /* Check if backlog is empty */
    if (bl->count <= 0)
        return -1;

    /* Dequeue one packet */
    int slot = bl->head;
    uint8_t *pkt = bl->slots[slot].data;
    uint16_t len = bl->slots[slot].len;

    bl->head = (bl->head + 1) % RPS_BACKLOG_SIZE;
    bl->count--;

    /* Dispatch the packet to the networking stack.
     * We import the internal dispatch functions from net.c.
     * For now, we call the existing single-threaded path since
     * the stack process backlog re-entry.
     */
    extern void net_rx_dispatch(const uint8_t *data, uint16_t len);
    net_rx_dispatch(pkt, len);

    /* Clear the slot data (optional, helps debugging) */
    memset(bl->slots[slot].data, 0, len);

    return 0;
}

/* ── RFS: Flow-to-CPU tracking ─────────────────────────────────────── */

/* Simple linear search over the flow table (small table, acceptable) */
static struct rfs_flow_entry *rfs_find_entry(const struct rps_flow_key *key)
{
    for (int i = 0; i < RFS_FLOW_TABLE_SIZE; i++) {
        struct rfs_flow_entry *e = &g_rfs_table.entries[i];
        if (!e->in_use) continue;
        if (e->key.src_ip  == key->src_ip  &&
            e->key.dst_ip  == key->dst_ip  &&
            e->key.src_port == key->src_port &&
            e->key.dst_port == key->dst_port &&
            e->key.proto   == key->proto) {
            return e;
        }
    }
    return NULL;
}

/* Find a free slot, or evict the oldest */
static struct rfs_flow_entry *rfs_alloc_entry(void)
{
    /* First pass: find an unused slot */
    for (int i = 0; i < RFS_FLOW_TABLE_SIZE; i++) {
        if (!g_rfs_table.entries[i].in_use) {
            g_rfs_table.count++;
            return &g_rfs_table.entries[i];
        }
    }

    /* Table full — evict the oldest entry (simple FIFO replacement) */
    int oldest = 0;
    uint64_t oldest_time = g_rfs_table.entries[0].last_seen;
    for (int i = 1; i < RFS_FLOW_TABLE_SIZE; i++) {
        if (g_rfs_table.entries[i].last_seen < oldest_time) {
            oldest = i;
            oldest_time = g_rfs_table.entries[i].last_seen;
        }
    }

    memset(&g_rfs_table.entries[oldest], 0,
           sizeof(struct rfs_flow_entry));
    return &g_rfs_table.entries[oldest];
}

int rfs_lookup_cpu(const struct rps_flow_key *key)
{
    if (!key) return -1;

    struct rfs_flow_entry *e = rfs_find_entry(key);
    if (!e) return -1;

    return e->last_cpu;
}

void rfs_record_flow(const struct rps_flow_key *key, int cpu)
{
    if (!key || cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;

    struct rfs_flow_entry *e = rfs_find_entry(key);
    if (!e) {
        /* Create new entry */
        e = rfs_alloc_entry();
        if (!e) return;
        e->key = *key;
        e->in_use = 1;
    }

    e->last_cpu = cpu;
    e->last_seen = 0;  /* No timer access needed for basic tracking */
}

void rfs_update_flow_cpu(const struct rps_flow_key *key)
{
    if (!key) return;

    int cpu = (int)get_cpu_id();
    rfs_record_flow(key, cpu);
}

/* ── Debug / stats ────────────────────────────────────────────────── */

int rps_backlog_count(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS) return -1;
    struct rps_backlog *bl = cpu_info_array[cpu].rps_backlog;
    if (!bl) return -1;
    return bl->count;
}

int rfs_flow_count(void)
{
    return g_rfs_table.count;
}
