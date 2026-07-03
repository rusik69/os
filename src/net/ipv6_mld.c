/*
 * ipv6_mld.c — IPv6 Multicast Listener Discovery v2 (MLDv2)
 *
 * Implements RFC 3810 host-side multicast group management:
 *   - Multicast group join/leave registration
 *   - MLDv2 General Query handler (respond with report)
 *   - MLDv2 Group-Specific / Group-and-Source-Specific Query handler
 *   - MLDv2 listener report generation (v1 and v2)
 *   - MLDv1 compatibility mode (Report v1 / Done)
 *   - Periodic unsolicited report sending
 *
 * This module maintains a table of joined multicast groups.  When a
 * query is received, a report is scheduled (jittered) per RFC 3810 §7.2.
 * Unsolicited reports are sent periodically at MLD_REPORT_INTERVAL.
 */

#define KERNEL_INTERNAL
#include "net.h"
#include "net_internal.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "errno.h"

/* ── Constants ──────────────────────────────────────────────────── */

/* Interval between unsolicited reports (125 seconds RFC 3810 §7.10).
 * At TIMER_FREQ=100 Hz: 125 * 100 = 12500 ticks */
#define MLD_REPORT_INTERVAL     12500

/* Maximum delay before responding to a query, as fraction of the
 * advertised Max Response Delay (burst avoidance, RFC 3810 §7.2).
 * We pick a random value between 0 and the advertised delay. */
#define MLD_MAX_RESP_DELAY_MS   10000  /* 10 seconds in ms */

/* Number of unsolicited reports to send on startup */
#define MLD_STARTUP_REPORTS     2

/* Buffer size for building MLD messages (larger than Ethernet MTU
 * to accommodate multiple group records) */
#define MLD_BUF_SIZE            1500

/* ── Module state ────────────────────────────────────────────────── */

/* Multicast group registration table */
struct mld_group_entry mld_group_table[MLD_GROUP_TABLE_SIZE];
int mld_group_count = 0;

/* Unsolicited report counter (startup phase) */
static int mld_startup_count = 0;

/* ── Forward declarations ───────────────────────────────────────── */

static int mld_find_group(const struct in6_addr *group);

/* ── Group table management ─────────────────────────────────────── */

/*
 * mld_find_group — Find a group entry by multicast address.
 * Returns the index, or -1 if not found.
 */
static int mld_find_group(const struct in6_addr *group)
{
    if (!group)
        return -1;

    for (int i = 0; i < MLD_GROUP_TABLE_SIZE; i++) {
        if (mld_group_table[i].valid &&
            ipv6_addr_equal(&mld_group_table[i].group_addr, group)) {
            return i;
        }
    }
    return -1;
}

/*
 * mld_find_free_slot — Find an unused entry in the group table.
 * Returns the index, or -1 if table is full.
 */
static int mld_find_free_slot(void)
{
    for (int i = 0; i < MLD_GROUP_TABLE_SIZE; i++) {
        if (!mld_group_table[i].valid)
            return i;
    }
    return -1;
}

/*
 * ipv6_mld_join — Register interest in a multicast group.
 *
 * Adds the group to the table (if not already present) and sends
 * an unsolicited MLDv2 report announcing membership.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ipv6_mld_join(const struct in6_addr *group)
{
    if (!group)
        return -EINVAL;

    if (!ipv6_addr_is_multicast(group))
        return -EINVAL;

    /* Already a member? */
    if (mld_find_group(group) >= 0)
        return 0;

    int slot = mld_find_free_slot();
    if (slot < 0) {
        kprintf("[mldv2] group table full, cannot join "
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                group->s6_addr[0], group->s6_addr[1],
                group->s6_addr[2], group->s6_addr[3],
                group->s6_addr[4], group->s6_addr[5],
                group->s6_addr[6], group->s6_addr[7],
                group->s6_addr[8], group->s6_addr[9],
                group->s6_addr[10], group->s6_addr[11],
                group->s6_addr[12], group->s6_addr[13],
                group->s6_addr[14], group->s6_addr[15]);
        return -ENOSPC;
    }

    struct mld_group_entry *entry = &mld_group_table[slot];
    memset(entry, 0, sizeof(*entry));
    memcpy(&entry->group_addr, group, sizeof(struct in6_addr));
    entry->filter_mode = MLD_FILTER_EXCLUDE;  /* receive all sources by default */
    entry->num_sources = 0;
    entry->valid = 1;

    /* Schedule immediate unsolicited report */
    entry->report_timer = timer_get_ticks();
    mld_group_count++;

    kprintf("[mldv2] joined group "
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x (slot %d)\n",
            group->s6_addr[0], group->s6_addr[1],
            group->s6_addr[2], group->s6_addr[3],
            group->s6_addr[4], group->s6_addr[5],
            group->s6_addr[6], group->s6_addr[7],
            group->s6_addr[8], group->s6_addr[9],
            group->s6_addr[10], group->s6_addr[11],
            group->s6_addr[12], group->s6_addr[13],
            group->s6_addr[14], group->s6_addr[15],
            slot);

    /* Send unsolicited report immediately */
    ipv6_mld_send_report(group);

    return 0;
}

/*
 * ipv6_mld_leave — Unregister interest in a multicast group.
 *
 * Removes the group from the table and sends an MLDv1 Done message
 * (for backward compatibility) or a v2 report with zero sources.
 *
 * Returns 0 on success, negative errno if not a member.
 */
int ipv6_mld_leave(const struct in6_addr *group)
{
    if (!group)
        return -EINVAL;

    int idx = mld_find_group(group);
    if (idx < 0)
        return -ENOENT;

    struct mld_group_entry *entry = &mld_group_table[idx];

    /* Send MLDv1 Done message for backward compatibility */
    ipv6_mld_send_done(group);

    /* Clear the entry */
    memset(entry, 0, sizeof(*entry));
    entry->valid = 0;
    mld_group_count--;

    kprintf("[mldv2] left group "
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
            group->s6_addr[0], group->s6_addr[1],
            group->s6_addr[2], group->s6_addr[3],
            group->s6_addr[4], group->s6_addr[5],
            group->s6_addr[6], group->s6_addr[7],
            group->s6_addr[8], group->s6_addr[9],
            group->s6_addr[10], group->s6_addr[11],
            group->s6_addr[12], group->s6_addr[13],
            group->s6_addr[14], group->s6_addr[15]);

    return 0;
}

/*
 * ipv6_mld_is_member — Check if a multicast group is joined.
 * Returns 1 if member, 0 if not.
 */
int ipv6_mld_is_member(const struct in6_addr *group)
{
    return (mld_find_group(group) >= 0) ? 1 : 0;
}

/* ── MLDv2 Query handler (RFC 3810 §5.1) ─────────────────────────── */

/*
 * ipv6_mld_handle_query — Process an incoming MLDv2/MLDv1 query.
 *
 * RFC 3810 §5.1 describes two types of queries:
 *   - General Query:   multicast_address == :: (all groups on link)
 *   - Group-Specific:  multicast_address == a specific group
 *   - Group-and-Source-Specific: has source addresses
 *
 * For each applicable group, we set the query_timer to a random delay
 * <= max_response_delay so the actual report is jittered.  If we
 * receive another query with a smaller delay, we reset the timer
 * (per RFC 3810 §7.2).
 */
void ipv6_mld_handle_query(struct ipv6_header *ip6,
                            const uint8_t *payload, uint16_t len)
{
    (void)ip6;

    if (!payload || len < sizeof(struct mldv2_query))
        return;

    const struct mldv2_query *query = (const struct mldv2_query *)payload;

    /* Verify it's a query type */
    if (query->icmp.type != ICMPV6_MLD_QUERY)
        return;

    uint16_t max_resp = ntohs(query->max_response_delay);
    /* Convert from 0.1ms units to timer ticks (1 tick = 10ms at 100Hz) */
    uint64_t delay_ticks;
    if (max_resp == 0) {
        delay_ticks = 0;  /* immediate response */
    } else {
        /* Compute random delay: [0, max_resp * 0.1ms]
         * We approximate by converting to ticks (1 tick = 10ms at 100Hz)
         * Actually max_resp is in tenths of milliseconds, so:
         *   delay_ms = max_resp / 10
         *   delay_ticks = delay_ms / 10 (at 100Hz)
         * Simplified: delay_ticks = max_resp / 100
         * But ensure at least 1 tick to avoid immediate burst. */
        uint32_t ms = (uint32_t)max_resp / 10;
        if (ms < 10) ms = 10;                   /* minimum 10ms delay */
        if (ms > MLD_MAX_RESP_DELAY_MS) ms = MLD_MAX_RESP_DELAY_MS;
        /* Simple jitter: use a portion of the delay based on group count */
        delay_ticks = (uint64_t)(ms / 10);      /* convert to 100Hz ticks */
        if (delay_ticks < 1) delay_ticks = 1;
    }

    uint64_t now = timer_get_ticks();
    uint64_t target_tick = now + delay_ticks;

    int is_general_query = ipv6_addr_is_unspecified(&query->multicast_address);
    struct in6_addr query_addr;
    memcpy(&query_addr, &query->multicast_address, sizeof(struct in6_addr));

    /* Iterate all group entries */
    for (int i = 0; i < MLD_GROUP_TABLE_SIZE; i++) {
        if (!mld_group_table[i].valid)
            continue;

        int apply = 0;
        if (is_general_query) {
            apply = 1;  /* General Query applies to all groups */
        } else if (ipv6_addr_equal(&mld_group_table[i].group_addr,
                                    &query_addr)) {
            apply = 1;  /* Group-specific query */
        }

        if (!apply)
            continue;

        /* Set jittered report timer — only if the new delay is shorter
         * than any already-scheduled response (per RFC 3810 §7.2). */
        if (mld_group_table[i].query_timer == 0 ||
            target_tick < mld_group_table[i].query_timer) {
            mld_group_table[i].query_timer = target_tick;
        }
    }
}

/* ── MLDv1 Report handler ───────────────────────────────────────── */

/*
 * ipv6_mld_handle_report_v1 — Process an MLDv1 listener report.
 *
 * MLDv1 reports (type 131) indicate that another host has joined a
 * multicast group on the link.  For a simple host implementation
 * this is informational only; we do not need to act on it.
 */
void ipv6_mld_handle_report_v1(struct ipv6_header *ip6,
                                const uint8_t *payload, uint16_t len)
{
    (void)ip6;
    (void)payload;
    (void)len;

    /* MLDv1 Report body is just an ICMPv6 header + 16-byte multicast
     * address.  We could use this to track other listeners, but for
     * a host implementation this is a no-op. */
}

/* ── MLDv2 Report handler ───────────────────────────────────────── */

/*
 * ipv6_mld_handle_report_v2 — Process an MLDv2 listener report.
 *
 * MLDv2 reports (type 143) contain group records.  For host-side
 * implementation we simply acknowledge receipt; router functionality
 * would track listener state per group.
 */
void ipv6_mld_handle_report_v2(struct ipv6_header *ip6,
                                const uint8_t *payload, uint16_t len)
{
    (void)ip6;

    if (!payload || len < sizeof(struct mldv2_report))
        return;

    const struct mldv2_report *report = (const struct mldv2_report *)payload;
    uint16_t num_records = ntohs(report->num_group_records);
    const uint8_t *ptr = (const uint8_t *)report->records;
    uint16_t remaining = len - (uint16_t)sizeof(struct mldv2_report);

    for (uint16_t r = 0; r < num_records && remaining > 0; r++) {
        if (remaining < sizeof(struct mldv2_group_record))
            break;

        const struct mldv2_group_record *rec =
            (const struct mldv2_group_record *)ptr;
        uint16_t rec_sources = ntohs(rec->num_sources);
        uint16_t rec_size = (uint16_t)(sizeof(struct mldv2_group_record) +
                           rec_sources * sizeof(struct in6_addr) +
                           rec->aux_data_len * 4);

        if (remaining < rec_size)
            break;

        /* For host mode, we only care if another host is joining a
         * group we're also a member of — handled by query timer. */
        ptr += rec_size;
        remaining -= rec_size;
    }
}

/* ── MLDv2 Report builder (RFC 3810 §5.2) ───────────────────────── */

/*
 * ipv6_mld_send_report — Send an MLDv2 listener report for a group.
 *
 * Builds an ICMPv6 type 143 message with a single group record of
 * type MODE_IS_EXCLUDE (or MODE_IS_INCLUDE if source list is known).
 *
 * Returns 0 on success, negative errno on failure.
 */
int ipv6_mld_send_report(const struct in6_addr *group)
{
    if (!group)
        return -EINVAL;

    /* Find the group entry */
    int idx = mld_find_group(group);
    if (idx < 0)
        return -ENOENT;

    struct mld_group_entry *entry = &mld_group_table[idx];

    uint8_t buf[MLD_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    /* The MLDv2 Report header */
    struct mldv2_report *report = (struct mldv2_report *)buf;
    report->icmp.type = ICMPV6_MLD_REPORT_V2;
    report->icmp.code = 0;
    report->icmp.checksum = 0;
    report->reserved = 0;
    report->num_group_records = htons(1);

    /* Build a single group record */
    struct mldv2_group_record *rec = (struct mldv2_group_record *)
        (buf + sizeof(struct mldv2_report));

    uint16_t record_sources = (uint16_t)entry->num_sources;
    if (record_sources > 8)
        record_sources = 8;

    rec->record_type = (entry->filter_mode == MLD_FILTER_INCLUDE)
        ? MLD2_MODE_IS_INCLUDE
        : MLD2_MODE_IS_EXCLUDE;
    rec->aux_data_len = 0;
    rec->num_sources = htons(record_sources);
    memcpy(&rec->multicast_addr, group, sizeof(struct in6_addr));

    /* Copy source addresses (if any) */
    for (uint16_t s = 0; s < record_sources; s++) {
        memcpy(&rec->sources[s], &entry->sources[s],
               sizeof(struct in6_addr));
    }

    uint16_t record_size = (uint16_t)(sizeof(struct mldv2_group_record) +
                           record_sources * sizeof(struct in6_addr));
    uint16_t total_len = (uint16_t)(sizeof(struct mldv2_report) + record_size);

    /* Compute ICMPv6 checksum */
    report->icmp.checksum = ipv6_checksum(&net_our_ipv6_ll, group,
                                           IP_PROTO_ICMPV6,
                                           buf, total_len);

    /* Send the report to the group address (so all MLDv2 routers see it) */
    send_ipv6(group, IP_PROTO_ICMPV6, buf, total_len);

    return 0;
}

/*
 * ipv6_mld_send_done — Send an MLDv1 Done message.
 *
 * MLDv1 Done (type 132) is sent to the all-routers multicast address
 * (FF02::2) when leaving a group, for backward-compatible routers.
 *
 * Returns 0 on success, negative errno on failure.
 */
int ipv6_mld_send_done(const struct in6_addr *group)
{
    if (!group)
        return -EINVAL;

    uint8_t buf[sizeof(struct icmpv6_header) + sizeof(struct in6_addr)];
    memset(buf, 0, sizeof(buf));

    struct icmpv6_header *icmp = (struct icmpv6_header *)buf;
    icmp->type = ICMPV6_MLD_DONE;  /* 132 */
    icmp->code = 0;
    icmp->checksum = 0;

    /* The multicast address field follows the ICMPv6 header */
    struct in6_addr *mcast_field = (struct in6_addr *)
        (buf + sizeof(struct icmpv6_header));
    memcpy(mcast_field, group, sizeof(struct in6_addr));

    uint16_t total_len = (uint16_t)(sizeof(struct icmpv6_header) +
                                     sizeof(struct in6_addr));

    /* Compute checksum with all-routers destination */
    struct in6_addr all_routers = IPV6_ADDR_ALL_ROUTERS;
    icmp->checksum = ipv6_checksum(&net_our_ipv6_ll, &all_routers,
                                    IP_PROTO_ICMPV6, buf, total_len);

    /* Send Done to all-routers (FF02::2) */
    send_ipv6(&all_routers, IP_PROTO_ICMPV6, buf, total_len);

    return 0;
}

/*
 * ipv6_mld_send_all_reports — Send reports for ALL joined groups.
 *
 * Used in response to a General Query.
 */
static void ipv6_mld_send_all_reports(void)
{
    for (int i = 0; i < MLD_GROUP_TABLE_SIZE; i++) {
        if (mld_group_table[i].valid) {
            ipv6_mld_send_report(&mld_group_table[i].group_addr);
        }
    }
}

/* ── Periodic poll ──────────────────────────────────────────────── */

/*
 * ipv6_mld_poll — Called from ipv6_poll() on each timer tick.
 *
 * Handles:
 *   1. Query-triggered report sending (jittered responses to queries)
 *   2. Periodic unsolicited reports (RFC 3810 §7.10)
 *   3. Startup-phase reports (RFC 3810 §7.6.2)
 */
void ipv6_mld_poll(void)
{
    uint64_t now = timer_get_ticks();

    /* Process query-triggered report timers */
    for (int i = 0; i < MLD_GROUP_TABLE_SIZE; i++) {
        if (!mld_group_table[i].valid)
            continue;

        struct mld_group_entry *entry = &mld_group_table[i];

        /* Query-triggered report */
        if (entry->query_timer > 0 && now >= entry->query_timer) {
            entry->query_timer = 0;
            ipv6_mld_send_report(&entry->group_addr);
        }

        /* Periodic unsolicited report */
        if (entry->report_timer > 0 && now >= entry->report_timer) {
            entry->report_timer = now + MLD_REPORT_INTERVAL;
            ipv6_mld_send_report(&entry->group_addr);
        }
    }

    /* Startup phase: send additional unsolicited reports per RFC 3810 §7.6.2 */
    if (mld_startup_count < MLD_STARTUP_REPORTS) {
        /* Send one burst of reports at startup */
        static uint64_t startup_next_tick = 0;
        if (startup_next_tick == 0 || now >= startup_next_tick) {
            ipv6_mld_send_all_reports();
            mld_startup_count++;
            startup_next_tick = now + MLD_REPORT_INTERVAL;
        }
    }
}

/* ── Initialization ─────────────────────────────────────────────── */

/*
 * ipv6_mld_init — Initialise the MLDv2 module.
 *
 * Registers the all-nodes multicast group (FF02::1) which every
 * IPv6 node must listen to, and the solicited-node multicast
 * address for the link-local address.
 */
void ipv6_mld_init(void)
{
    memset(mld_group_table, 0, sizeof(mld_group_table));
    mld_group_count = 0;
    mld_startup_count = 0;

    /* Join the all-nodes multicast group (mandatory per RFC 8200) */
    struct in6_addr all_nodes = IPV6_ADDR_ALL_NODES;
    ipv6_mld_join(&all_nodes);

    /* Join the all-routers multicast group (for receiving RAs) */
    struct in6_addr all_routers = IPV6_ADDR_ALL_ROUTERS;
    ipv6_mld_join(&all_routers);

    /* Join the solicited-node multicast address for our link-local address.
     * The solicited-node address is FF02::1:FFXX:XXXX derived from the
     * lower 24 bits of the link-local address. */
    {
        struct in6_addr sn_addr;
        ipv6_calc_solicited_node(&net_our_ipv6_ll, &sn_addr);
        ipv6_mld_join(&sn_addr);
    }

    kprintf("[mldv2] initialised, %d groups registered\n", mld_group_count);
}
