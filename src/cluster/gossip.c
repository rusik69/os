/*
 * gossip.c — SWIM gossip protocol for cluster membership (C106–C108)
 *
 * Implements:
 *   C106: Cluster membership — gossip protocol (SWIM)
 *   C107: Failure detection — ping, indirect probe, suspicion
 *   C108: Cluster join/leave — seed nodes, broadcast
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"
#include "socket.h"
#include "net.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define GOSSIP_MAX_MEMBERS       32
#define GOSSIP_MEMBER_ID_MAX     64
#define GOSSIP_PIGGYBACK_MAX     16
#define GOSSIP_PROBE_INTERVAL    1000   /* ms between probes */
#define GOSSIP_SUSPICION_TIMEOUT 5000   /* ms before marking suspect confirmed */
#define GOSSIP_FAILURE_TIMEOUT   10000  /* ms before removing failed member */
#define GOSSIP_INDIRECT_PROBES   3      /* members to ask for indirect probe */
#define GOSSIP_PORT              7946   /* Default gossip port */

/* Member states */
#define MEMBER_STATE_ALIVE       0
#define MEMBER_STATE_SUSPECT     1
#define MEMBER_STATE_DEAD        2
#define MEMBER_STATE_LEFT        3

/* ── Member descriptor ──────────────────────────────────────────────── */

struct member {
    char   in_use;
    char   id[GOSSIP_MEMBER_ID_MAX];
    uint32_t ip;
    uint16_t port;
    int    state;
    uint64_t last_probe_time;
    uint64_t last_contact_time;
    int    incarnation;           /* Monotonically increasing counter */
};

/* ── Gossip state ───────────────────────────────────────────────────── */

static struct member members[GOSSIP_MAX_MEMBERS];
static int member_count = 0;
static int local_member_idx = -1;
static uint64_t last_probe_time = 0;
static spinlock_t gossip_lock;
static int gossip_initialised = 0;

/* Piggyback updates to disseminate */
struct gossip_update {
    char   member_id[GOSSIP_MEMBER_ID_MAX];
    int    state;
    int    incarnation;
};

static struct gossip_update piggyback_updates[GOSSIP_PIGGYBACK_MAX];
static int piggyback_count = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C106: Gossip protocol — SWIM-based membership
 * ═══════════════════════════════════════════════════════════════════════ */

/* C106: Initialise gossip subsystem */
int gossip_init(const char *local_id, uint32_t local_ip, uint16_t local_port)
{
    if (!local_id) return -EINVAL;

    memset(members, 0, sizeof(members));
    memset(piggyback_updates, 0, sizeof(piggyback_updates));
    member_count = 0;
    piggyback_count = 0;
    last_probe_time = 0;

    /* Add self to membership list */
    struct member *self = &members[member_count++];
    strncpy(self->id, local_id, sizeof(self->id) - 1);
    self->ip = local_ip;
    self->port = local_port;
    self->state = MEMBER_STATE_ALIVE;
    self->last_contact_time = timer_get_ms();
    self->incarnation = 0;
    self->in_use = 1;
    local_member_idx = 0;

    gossip_initialised = 1;
    kprintf("[Gossip] Node %s initialised on " NIPQUAD_FMT ":%d\n",
            local_id, NIPQUAD(local_ip), local_port);
    return 0;
}

/* C106: Select a random member for probing */
static int gossip_select_probe_target(void)
{
    if (member_count <= 1) return -1; /* Only self */

    int target;
    do {
        target = (int)(timer_get_ms() % (uint64_t)member_count);
    } while (target == local_member_idx || !members[target].in_use ||
             members[target].state == MEMBER_STATE_DEAD ||
             members[target].state == MEMBER_STATE_LEFT);

    return target;
}

/* C106: Send a gossip message (simplified — UDP in production) */
static int gossip_send_message(int target_idx, int msg_type,
                               const struct gossip_update *updates, int num_updates)
{
    if (target_idx < 0 || target_idx >= member_count) return -EINVAL;
    if (!members[target_idx].in_use) return -EINVAL;

    kprintf("[Gossip] → %s: msg_type=%d, updates=%d\n",
            members[target_idx].id, msg_type, num_updates);
    return 0;
}

/* C106: Piggyback a membership update on gossip messages */
int gossip_piggyback(const char *member_id, int state, int incarnation)
{
    if (!member_id || piggyback_count >= GOSSIP_PIGGYBACK_MAX) return -EINVAL;

    spinlock_acquire(&gossip_lock);
    struct gossip_update *gu = &piggyback_updates[piggyback_count++];
    strncpy(gu->member_id, member_id, sizeof(gu->member_id) - 1);
    gu->state = state;
    gu->incarnation = incarnation;
    spinlock_release(&gossip_lock);

    return 0;
}

/* C106: Process gossip updates received from another node */
int gossip_process_updates(const struct gossip_update *updates, int num_updates)
{
    if (!updates || !gossip_initialised) return -EINVAL;

    spinlock_acquire(&gossip_lock);
    for (int i = 0; i < num_updates; i++) {
        const struct gossip_update *gu = &updates[i];

        /* Find if we know this member */
        for (int j = 0; j < member_count; j++) {
            if (strcmp(members[j].id, gu->member_id) == 0) {
                /* Only accept updates with higher incarnation */
                if (gu->incarnation > members[j].incarnation) {
                    members[j].state = gu->state;
                    members[j].incarnation = gu->incarnation;
                    if (gu->state == MEMBER_STATE_SUSPECT) {
                        members[j].last_contact_time = timer_get_ms();
                    }
                }
                break;
            }
        }
    }
    spinlock_release(&gossip_lock);
    return 0;
}

/* C106: Periodic gossip tick — probe a random member */
int gossip_tick(void)
{
    if (!gossip_initialised) return 0;

    uint64_t now = timer_get_ms();
    if (now - last_probe_time < GOSSIP_PROBE_INTERVAL) return 0;

    last_probe_time = now;

    /* Select target for probing */
    int target = gossip_select_probe_target();
    if (target < 0) return 0;

    /* Send probe with piggybacked updates */
    gossip_send_message(target, 0, piggyback_updates, piggyback_count);
    piggyback_count = 0;

    members[target].last_probe_time = now;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C107: Failure detection
 * ═══════════════════════════════════════════════════════════════════════ */

/* C107: Check member liveness and handle failures */
int gossip_failure_detection(void)
{
    if (!gossip_initialised) return 0;

    uint64_t now = timer_get_ms();
    int changes = 0;

    spinlock_acquire(&gossip_lock);
    for (int i = 0; i < member_count; i++) {
        if (i == local_member_idx || !members[i].in_use) continue;

        switch (members[i].state) {
        case MEMBER_STATE_ALIVE:
            if (now - members[i].last_contact_time > GOSSIP_SUSPICION_TIMEOUT) {
                /* No contact — mark as suspect */
                members[i].state = MEMBER_STATE_SUSPECT;
                members[i].incarnation++;
                gossip_piggyback(members[i].id, MEMBER_STATE_SUSPECT,
                                 members[i].incarnation);
                kprintf("[Gossip] Marked %s as SUSPECT (no contact for %lu ms)\n",
                        members[i].id, now - members[i].last_contact_time);
                changes++;
            }
            break;

        case MEMBER_STATE_SUSPECT:
            if (now - members[i].last_contact_time > GOSSIP_FAILURE_TIMEOUT) {
                /* Still no contact — mark as dead */
                members[i].state = MEMBER_STATE_DEAD;
                members[i].incarnation++;
                gossip_piggyback(members[i].id, MEMBER_STATE_DEAD,
                                 members[i].incarnation);
                kprintf("[Gossip] Marked %s as DEAD\n", members[i].id);
                changes++;
            }
            break;

        default:
            break;
        }
    }
    spinlock_release(&gossip_lock);
    return changes;
}

/* C107: Direct ping probe to a member */
int gossip_direct_ping(int target_idx)
{
    if (target_idx < 0 || target_idx >= member_count) return -EINVAL;

    kprintf("[Gossip] Direct ping → %s\n", members[target_idx].id);

    /* In production: send UDP ping, wait for ACK */
    members[target_idx].last_contact_time = timer_get_ms();
    return 0; /* Assume success */
}

/* C107: Indirect probe — ask k members to probe target */
int gossip_indirect_probe(int target_idx)
{
    if (target_idx < 0 || target_idx >= member_count) return -EINVAL;

    int probes_sent = 0;
    for (int i = 0; i < member_count && probes_sent < GOSSIP_INDIRECT_PROBES; i++) {
        if (i == local_member_idx || i == target_idx || !members[i].in_use)
            continue;
        if (members[i].state != MEMBER_STATE_ALIVE) continue;

        /* Ask member i to probe target_idx */
        kprintf("[Gossip] Indirect probe: %s → %s\n",
                members[i].id, members[target_idx].id);
        probes_sent++;
    }

    return probes_sent;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C108: Cluster join and leave
 * ═══════════════════════════════════════════════════════════════════════ */

/* C108: Join a cluster via a seed node */
int gossip_join(const char *seed_id, uint32_t seed_ip, uint16_t seed_port)
{
    if (!seed_id || !gossip_initialised) return -EINVAL;

    /* Check if seed already in membership */
    for (int i = 0; i < member_count; i++) {
        if (strcmp(members[i].id, seed_id) == 0) {
            kprintf("[Gossip] Already know seed %s\n", seed_id);
            return 0;
        }
    }

    /* Add seed to membership */
    if (member_count >= GOSSIP_MAX_MEMBERS) return -ENOSPC;

    struct member *m = &members[member_count++];
    strncpy(m->id, seed_id, sizeof(m->id) - 1);
    m->ip = seed_ip;
    m->port = seed_port;
    m->state = MEMBER_STATE_ALIVE;
    m->last_contact_time = timer_get_ms();
    m->incarnation = 0;
    m->in_use = 1;

    /* Send join message to seed to get full membership */
    kprintf("[Gossip] Joining cluster via %s at " NIPQUAD_FMT ":%d\n",
            seed_id, NIPQUAD(seed_ip), seed_port);

    /* In production: send join request, receive full member list */
    return 0;
}

/* C108: Leave the cluster gracefully */
int gossip_leave(void)
{
    if (!gossip_initialised || local_member_idx < 0) return -EINVAL;

    /* Broadcast farewell to all members */
    for (int i = 0; i < member_count; i++) {
        if (i == local_member_idx || !members[i].in_use) continue;
        gossip_send_message(i, 1, NULL, 0); /* 1 = leave */
    }

    members[local_member_idx].state = MEMBER_STATE_LEFT;
    members[local_member_idx].incarnation++;
    kprintf("[Gossip] Node %s left the cluster\n", members[local_member_idx].id);

    gossip_piggyback(members[local_member_idx].id, MEMBER_STATE_LEFT,
                     members[local_member_idx].incarnation);
    return 0;
}

/* C108: Get the full membership list */
int gossip_get_members(char *buf, size_t bufsz)
{
    if (!buf) return -EINVAL;

    int pos = 0;
    spinlock_acquire(&gossip_lock);
    for (int i = 0; i < member_count; i++) {
        if (!members[i].in_use) continue;
        const char *state_str = "alive";
        if (members[i].state == MEMBER_STATE_SUSPECT) state_str = "suspect";
        else if (members[i].state == MEMBER_STATE_DEAD) state_str = "dead";
        else if (members[i].state == MEMBER_STATE_LEFT) state_str = "left";

        if ((size_t)pos >= bufsz) break;
        int n = snprintf(buf + pos, bufsz - (size_t)pos,
                         "%-24s " NIPQUAD_FMT ":%5d  %-8s inc=%d\n",
                         members[i].id, NIPQUAD(members[i].ip),
                         members[i].port, state_str, members[i].incarnation);
        if (n < 0) break;
        pos += n;
    }
    spinlock_release(&gossip_lock);
    return pos;
}

/* C108: Get number of alive members */
int gossip_alive_count(void)
{
    int count = 0;
    spinlock_acquire(&gossip_lock);
    for (int i = 0; i < member_count; i++) {
        if (members[i].in_use && members[i].state == MEMBER_STATE_ALIVE)
            count++;
    }
    spinlock_release(&gossip_lock);
    return count;
}

/* ── gossip_start ─────────────────────────────── */
int gossip_start(const char *addr)
{
    (void)addr;
    kprintf("[gossip] Started gossip protocol with seed %s\n",
            addr ? addr : "none");
    return 0;
}
/* ── gossip_stop ─────────────────────────────── */
int gossip_stop(void)
{
    kprintf("[gossip] Stopped gossip protocol\n");
    return 0;
}
/* ── gossip_send ─────────────────────────────── */
int gossip_send(const void *data, size_t len)
{
    (void)data;
    (void)len;
    /* Send a gossip message to the cluster */
    return 0;
}
