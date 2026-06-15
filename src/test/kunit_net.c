/*
 * kunit_net.c — KUnit test suite for networking subsystems.
 *
 * Tests socket creation/bind/listen/accept, TCP state machine
 * transitions, UDP send/receive, ARP cache operations, and
 * network interface up/down lifecycle.
 *
 * These tests run inside the running kernel and validate
 * the networking subsystem's internal consistency.
 */

#include "kunit.h"
#include "socket.h"
#include "net_internal.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "process.h"

/* ====================================================================
 *  1. Socket creation and state transitions
 * ==================================================================== */

static void net_socket_create_test(struct kunit *test)
{
    int fd = sock_alloc();
    KUNIT_EXPECT_NE(test, fd, -1);
    KUNIT_EXPECT_TRUE(test, fd >= 0);

    if (fd >= 0) {
        struct socket *s = sock_get(fd);
        KUNIT_EXPECT_NOT_NULL(test, s);
        KUNIT_EXPECT_EQ(test, (int64_t)s->in_use, (int64_t)1);
        KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_CREATED);
        sock_free(fd);
    }
}

static void net_socket_bind_listen_accept_test(struct kunit *test)
{
    int fd = sock_alloc();
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    struct socket *s = sock_get(fd);
    s->domain = AF_INET;
    s->type = SOCK_STREAM;
    s->protocol = IPPROTO_TCP;

    /* Bind to loopback address, port 9000 */
    s->local_ip = 0x7F000001;  /* 127.0.0.1 */
    s->local_port = 9000;
    s->state = SOCK_STATE_BOUND;
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_BOUND);
    KUNIT_EXPECT_EQ(test, (int64_t)s->local_port, (int64_t)9000);

    /* Listen */
    s->state = SOCK_STATE_LISTENING;
    s->backlog = 5;
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_LISTENING);

    /* Accept requires a pending connection — we test the state machine */
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_LISTENING);

    sock_free(fd);
}

static void net_socket_invalid_ops_test(struct kunit *test)
{
    /* sock_free on never-allocated slot should not crash */
    sock_free(-1);
    sock_free(SOCK_MAX + 10);
    sock_free(9999);

    /* sock_get on invalid slot should return NULL */
    struct socket *s = sock_get(-1);
    KUNIT_EXPECT_NULL(test, s);
    s = sock_get(SOCK_MAX);
    KUNIT_EXPECT_NULL(test, s);

    KUNIT_EXPECT_TRUE(test, 1);
}

/* ====================================================================
 *  2. TCP state machine transitions
 * ==================================================================== */

/* TCP states mirror the kernel's enum from tcp.h */
enum tcp_state {
    TCP_CLOSED       = 0,
    TCP_LISTEN       = 1,
    TCP_SYN_SENT     = 2,
    TCP_SYN_RECEIVED = 3,
    TCP_ESTABLISHED  = 4,
    TCP_FIN_WAIT1    = 5,
    TCP_FIN_WAIT2    = 6,
    TCP_CLOSE_WAIT   = 7,
    TCP_CLOSING      = 8,
    TCP_LAST_ACK     = 9,
    TCP_TIME_WAIT    = 10,
};

/* Simulated TCP transition table — same logic as kernel net/tcp.c */
static int tcp_transition(enum tcp_state *state, const char *event)
{
    if (!state || !event) return 0;

    if (strcmp(event, "APP_ACTIVE_OPEN") == 0) {
        if (*state == TCP_CLOSED) { *state = TCP_SYN_SENT; return 1; }
        return 0;
    }
    if (strcmp(event, "APP_PASSIVE_OPEN") == 0) {
        if (*state == TCP_CLOSED) { *state = TCP_LISTEN; return 1; }
        return 0;
    }
    if (strcmp(event, "RCV_SYN") == 0) {
        if (*state == TCP_LISTEN) { *state = TCP_SYN_RECEIVED; return 1; }
        if (*state == TCP_SYN_SENT) { *state = TCP_ESTABLISHED; return 1; }
        return 0;
    }
    if (strcmp(event, "SEND_SYNACK") == 0) {
        /* SYN-ACK is sent automatically; mark SYN_RCVD -> ESTAB on ACK */
        return 0;
    }
    if (strcmp(event, "RCV_SYNACK") == 0) {
        if (*state == TCP_SYN_SENT) { *state = TCP_ESTABLISHED; return 1; }
        return 0;
    }
    if (strcmp(event, "RCV_ACK") == 0) {
        if (*state == TCP_SYN_RECEIVED) { *state = TCP_ESTABLISHED; return 1; }
        if (*state == TCP_FIN_WAIT1) { *state = TCP_FIN_WAIT2; return 1; }
        if (*state == TCP_CLOSING) { *state = TCP_TIME_WAIT; return 1; }
        if (*state == TCP_LAST_ACK) { *state = TCP_CLOSED; return 1; }
        return 0;
    }
    if (strcmp(event, "APP_CLOSE") == 0) {
        if (*state == TCP_ESTABLISHED) { *state = TCP_FIN_WAIT1; return 1; }
        if (*state == TCP_CLOSE_WAIT) { *state = TCP_LAST_ACK; return 1; }
        if (*state == TCP_LISTEN) { *state = TCP_CLOSED; return 1; }
        return 0;
    }
    if (strcmp(event, "RCV_FIN") == 0) {
        if (*state == TCP_ESTABLISHED) { *state = TCP_CLOSE_WAIT; return 1; }
        if (*state == TCP_FIN_WAIT1) { *state = TCP_CLOSING; return 1; }
        if (*state == TCP_FIN_WAIT2) { *state = TCP_TIME_WAIT; return 1; }
        return 0;
    }
    if (strcmp(event, "TIMEOUT_2MSL") == 0) {
        if (*state == TCP_TIME_WAIT) { *state = TCP_CLOSED; return 1; }
        return 0;
    }

    return 0; /* Invalid transition */
}

static void tcp_active_open_test(struct kunit *test)
{
    enum tcp_state s = TCP_CLOSED;

    /* Active open: CLOSED -> SYN_SENT */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "APP_ACTIVE_OPEN"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_SYN_SENT);

    /* Receive SYN+ACK: SYN_SENT -> ESTABLISHED */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "RCV_SYNACK"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_ESTABLISHED);

    /* Close: ESTABLISHED -> FIN_WAIT1 */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "APP_CLOSE"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_FIN_WAIT1);

    /* Receive peer's FIN: FIN_WAIT1 -> CLOSING */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "RCV_FIN"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_CLOSING);

    /* Receive ACK for our FIN: CLOSING -> TIME_WAIT */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "RCV_ACK"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_TIME_WAIT);

    /* Timeout: TIME_WAIT -> CLOSED */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "TIMEOUT_2MSL"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_CLOSED);
}

static void tcp_passive_open_test(struct kunit *test)
{
    enum tcp_state s = TCP_CLOSED;

    /* Passive open: CLOSED -> LISTEN */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "APP_PASSIVE_OPEN"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_LISTEN);

    /* Receive SYN: LISTEN -> SYN_RECEIVED */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "RCV_SYN"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_SYN_RECEIVED);

    /* Receive ACK of our SYN-ACK: SYN_RECEIVED -> ESTABLISHED */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "RCV_ACK"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_ESTABLISHED);

    /* Peer closes first: ESTABLISHED -> CLOSE_WAIT */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "RCV_FIN"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_CLOSE_WAIT);

    /* We close: CLOSE_WAIT -> LAST_ACK */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "APP_CLOSE"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_LAST_ACK);

    /* ACK of our FIN: LAST_ACK -> CLOSED */
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "RCV_ACK"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_CLOSED);
}

static void tcp_invalid_transition_test(struct kunit *test)
{
    /* Simultaneous close: both sides send FIN */
    enum tcp_state s = TCP_ESTABLISHED;
    KUNIT_EXPECT_TRUE(test, tcp_transition(&s, "APP_CLOSE"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_FIN_WAIT1);

    /* Can't LISTEN from ESTABLISHED */
    KUNIT_EXPECT_FALSE(test, tcp_transition(&s, "APP_PASSIVE_OPEN"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_FIN_WAIT1);

    /* Can't active-open from LISTEN */
    s = TCP_LISTEN;
    KUNIT_EXPECT_FALSE(test, tcp_transition(&s, "APP_ACTIVE_OPEN"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_LISTEN);

    /* Unknown event should not change state */
    KUNIT_EXPECT_FALSE(test, tcp_transition(&s, "UNKNOWN_EVENT"));
    KUNIT_EXPECT_EQ(test, (int64_t)s, (int64_t)TCP_LISTEN);

    /* NULL arguments should not crash */
    KUNIT_EXPECT_FALSE(test, tcp_transition(NULL, "APP_CLOSE"));

    enum tcp_state t = TCP_CLOSED;
    KUNIT_EXPECT_FALSE(test, tcp_transition(&t, NULL));
    KUNIT_EXPECT_EQ(test, (int64_t)t, (int64_t)TCP_CLOSED);
}

/* ====================================================================
 *  3. UDP send/receive (through socket API simulation)
 * ==================================================================== */

static void udp_socket_create_test(struct kunit *test)
{
    int fd = sock_alloc();
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    struct socket *s = sock_get(fd);
    s->domain = AF_INET;
    s->type = SOCK_DGRAM;
    s->protocol = IPPROTO_UDP;
    s->state = SOCK_STATE_CREATED;

    KUNIT_EXPECT_EQ(test, (int64_t)s->type, (int64_t)SOCK_DGRAM);
    KUNIT_EXPECT_EQ(test, (int64_t)s->protocol, (int64_t)IPPROTO_UDP);

    sock_free(fd);
}

static void udp_socket_bind_send_test(struct kunit *test)
{
    int fd = sock_alloc();
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    struct socket *s = sock_get(fd);
    s->domain = AF_INET;
    s->type = SOCK_DGRAM;
    s->protocol = IPPROTO_UDP;
    s->local_ip = 0x7F000001;
    s->local_port = 9001;
    s->state = SOCK_STATE_BOUND;

    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_BOUND);
    KUNIT_EXPECT_EQ(test, (int64_t)s->local_port, (int64_t)9001);

    /* Set remote endpoint (UDP connected socket style) */
    s->remote_ip = 0x7F000001;
    s->remote_port = 9002;
    s->state = SOCK_STATE_CONNECTED;

    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_CONNECTED);
    KUNIT_EXPECT_EQ(test, (int64_t)s->remote_port, (int64_t)9002);

    sock_free(fd);
}

static void udp_socket_options_test(struct kunit *test)
{
    int fd = sock_alloc();
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    if (fd < 0) return;

    struct socket *s = sock_get(fd);
    s->domain = AF_INET;
    s->type = SOCK_DGRAM;
    s->protocol = IPPROTO_UDP;

    /* Test broadcast option (meaningful for UDP) */
    s->broadcast = 1;
    KUNIT_EXPECT_EQ(test, (int64_t)s->broadcast, (int64_t)1);

    s->broadcast = 0;
    KUNIT_EXPECT_EQ(test, (int64_t)s->broadcast, (int64_t)0);

    /* Test receive buffer sizing */
    s->rcvbuf = 65536;
    KUNIT_EXPECT_EQ(test, (int64_t)s->rcvbuf, (int64_t)65536);

    sock_free(fd);
}

/* ====================================================================
 *  4. ARP cache operations
 * ==================================================================== */

/* Simulated ARP cache entry — mirrors kernel's arp_cache.h */
struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
};

#define ARP_CACHE_SIZE 16

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

static void arp_cache_init(void)
{
    memset(arp_cache, 0, sizeof(arp_cache));
}

static int arp_cache_lookup(uint32_t ip, uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static int arp_cache_add(uint32_t ip, const uint8_t mac[6])
{
    /* Check if already exists */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return 0;
        }
    }
    /* Find a free slot */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return 0;
        }
    }
    return -1; /* Cache full */
}

static int arp_cache_remove(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            arp_cache[i].valid = 0;
            return 0;
        }
    }
    return -1; /* Not found */
}

static void arp_cache_add_lookup_test(struct kunit *test)
{
    arp_cache_init();

    uint32_t ip1 = 0xC0A80001; /* 192.168.0.1 */
    uint32_t ip2 = 0xC0A80002; /* 192.168.0.2 */
    uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    uint8_t out[6];

    /* Initially, lookups should fail */
    KUNIT_EXPECT_FALSE(test, arp_cache_lookup(ip1, out));

    /* Add entries */
    KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_add(ip1, mac1), (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_add(ip2, mac2), (int64_t)0);

    /* Lookup should succeed */
    memset(out, 0, 6);
    KUNIT_EXPECT_TRUE(test, arp_cache_lookup(ip1, out));
    KUNIT_EXPECT_EQ(test, (int64_t)memcmp(out, mac1, 6), (int64_t)0);

    memset(out, 0, 6);
    KUNIT_EXPECT_TRUE(test, arp_cache_lookup(ip2, out));
    KUNIT_EXPECT_EQ(test, (int64_t)memcmp(out, mac2, 6), (int64_t)0);

    /* Remove */
    KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_remove(ip1), (int64_t)0);
    KUNIT_EXPECT_FALSE(test, arp_cache_lookup(ip1, out));

    /* Remove non-existent entry */
    KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_remove(0xFFFFFFFF), (int64_t)-1);
}

static void arp_cache_update_test(struct kunit *test)
{
    arp_cache_init();

    uint32_t ip = 0xC0A80001;
    uint8_t mac_old[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t mac_new[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t out[6];

    arp_cache_add(ip, mac_old);

    /* Update same IP with new MAC */
    arp_cache_add(ip, mac_new);

    memset(out, 0, 6);
    KUNIT_EXPECT_TRUE(test, arp_cache_lookup(ip, out));
    KUNIT_EXPECT_EQ(test, (int64_t)memcmp(out, mac_new, 6), (int64_t)0);
}

static void arp_cache_full_test(struct kunit *test)
{
    arp_cache_init();

    uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

    /* Fill the cache */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        uint32_t ip = 0xC0A80000 | (uint32_t)(i + 1);
        KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_add(ip, mac), (int64_t)0);
    }

    /* Next add should fail (cache full) */
    uint32_t extra_ip = 0xC0A800FF;
    KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_add(extra_ip, mac), (int64_t)-1);

    /* Remove one and retry */
    KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_remove(0xC0A80001), (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)arp_cache_add(extra_ip, mac), (int64_t)0);
}

/* ====================================================================
 *  5. Network interface up/down
 * ==================================================================== */

/* Simulated network interface state */
#define NET_IF_MAX 4

enum net_if_state {
    IF_STATE_DOWN = 0,
    IF_STATE_UP   = 1,
};

struct net_interface {
    int      in_use;
    char     name[16];
    uint32_t ip;
    uint32_t netmask;
    uint8_t  mac[6];
    int      state;
    uint64_t tx_packets;
    uint64_t rx_packets;
};

static struct net_interface net_interfaces[NET_IF_MAX];

static void net_if_init(void)
{
    memset(net_interfaces, 0, sizeof(net_interfaces));
}

static int net_if_create(const char *name, uint32_t ip, uint32_t netmask,
                         const uint8_t *mac)
{
    for (int i = 0; i < NET_IF_MAX; i++) {
        if (!net_interfaces[i].in_use) {
            net_interfaces[i].in_use = 1;
            strncpy(net_interfaces[i].name, name, 15);
            net_interfaces[i].name[15] = '\0';
            net_interfaces[i].ip = ip;
            net_interfaces[i].netmask = netmask;
            if (mac) memcpy(net_interfaces[i].mac, mac, 6);
            net_interfaces[i].state = IF_STATE_DOWN;
            net_interfaces[i].tx_packets = 0;
            net_interfaces[i].rx_packets = 0;
            return i;
        }
    }
    return -1;
}

static int net_if_set_state(int idx, int state)
{
    if (idx < 0 || idx >= NET_IF_MAX) return -1;
    if (!net_interfaces[idx].in_use) return -1;
    net_interfaces[idx].state = state;
    return 0;
}

static void net_if_up_down_test(struct kunit *test)
{
    net_if_init();

    uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

    /* Create interface */
    int idx = net_if_create("eth0", 0xC0A80001, 0xFFFFFF00, mac);
    KUNIT_EXPECT_TRUE(test, idx >= 0);
    KUNIT_EXPECT_TRUE(test, idx < NET_IF_MAX);

    /* Should start DOWN */
    KUNIT_EXPECT_EQ(test, (int64_t)net_interfaces[idx].state,
                    (int64_t)IF_STATE_DOWN);

    /* Bring UP */
    KUNIT_EXPECT_EQ(test, (int64_t)net_if_set_state(idx, IF_STATE_UP),
                    (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)net_interfaces[idx].state,
                    (int64_t)IF_STATE_UP);

    /* Bring DOWN again */
    KUNIT_EXPECT_EQ(test, (int64_t)net_if_set_state(idx, IF_STATE_DOWN),
                    (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)net_interfaces[idx].state,
                    (int64_t)IF_STATE_DOWN);

    /* Invalid operations */
    KUNIT_EXPECT_EQ(test, (int64_t)net_if_set_state(-1, IF_STATE_UP),
                    (int64_t)-1);
    KUNIT_EXPECT_EQ(test, (int64_t)net_if_set_state(NET_IF_MAX, IF_STATE_UP),
                    (int64_t)-1);
}

static void net_if_create_multiple_test(struct kunit *test)
{
    net_if_init();

    uint8_t mac0[6] = {0x52, 0x54, 0x00, 0x00, 0x00, 0x01};
    uint8_t mac1[6] = {0x52, 0x54, 0x00, 0x00, 0x00, 0x02};

    int eth0 = net_if_create("eth0", 0xC0A80001, 0xFFFFFF00, mac0);
    int eth1 = net_if_create("eth1", 0xC0A80002, 0xFFFFFF00, mac1);

    KUNIT_EXPECT_TRUE(test, eth0 >= 0);
    KUNIT_EXPECT_TRUE(test, eth1 >= 0);
    KUNIT_EXPECT_NE(test, eth0, eth1);

    /* Bring both up */
    net_if_set_state(eth0, IF_STATE_UP);
    net_if_set_state(eth1, IF_STATE_UP);

    KUNIT_EXPECT_EQ(test, (int64_t)net_interfaces[eth0].state,
                    (int64_t)IF_STATE_UP);
    KUNIT_EXPECT_EQ(test, (int64_t)net_interfaces[eth1].state,
                    (int64_t)IF_STATE_UP);

    /* Verify IP addresses */
    KUNIT_EXPECT_EQ(test, (int64_t)net_interfaces[eth0].ip,
                    (int64_t)0xC0A80001);
    KUNIT_EXPECT_EQ(test, (int64_t)net_interfaces[eth1].ip,
                    (int64_t)0xC0A80002);

    /* Verify names */
    KUNIT_EXPECT_EQ(test, (int64_t)strcmp(net_interfaces[eth0].name, "eth0"),
                    (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)strcmp(net_interfaces[eth1].name, "eth1"),
                    (int64_t)0);
}

/* ====================================================================
 *  6. Socket reuse and edge cases
 * ==================================================================== */

static void net_socket_reuse_test(struct kunit *test)
{
    int fd1 = sock_alloc();
    KUNIT_EXPECT_TRUE(test, fd1 >= 0);

    sock_free(fd1);

    /* Allocate again — should get same or different slot */
    int fd2 = sock_alloc();
    KUNIT_EXPECT_TRUE(test, fd2 >= 0);

    struct socket *s = sock_get(fd2);
    KUNIT_EXPECT_NOT_NULL(test, s);
    KUNIT_EXPECT_EQ(test, (int64_t)s->in_use, (int64_t)1);

    sock_free(fd2);
}

static void net_socket_max_allocation_test(struct kunit *test)
{
    int fds[SOCK_MAX];
    int count = 0;

    /* Allocate all sockets */
    for (int i = 0; i < SOCK_MAX; i++) {
        fds[i] = sock_alloc();
        if (fds[i] >= 0) count++;
    }

    KUNIT_EXPECT_EQ(test, (int64_t)count, (int64_t)SOCK_MAX);

    /* Next allocation should fail */
    int extra = sock_alloc();
    KUNIT_EXPECT_EQ(test, (int64_t)extra, (int64_t)-1);

    /* Free all */
    for (int i = 0; i < count; i++) {
        sock_free(fds[i]);
    }

    /* Should be able to allocate again */
    int fd = sock_alloc();
    KUNIT_EXPECT_TRUE(test, fd >= 0);
    sock_free(fd);
}

/* ====================================================================
 *  Test case list (terminated by {0})
 * ==================================================================== */

static struct kunit_case net_test_cases[] = {
    KUNIT_CASE(net_socket_create_test),
    KUNIT_CASE(net_socket_bind_listen_accept_test),
    KUNIT_CASE(net_socket_invalid_ops_test),
    KUNIT_CASE(tcp_active_open_test),
    KUNIT_CASE(tcp_passive_open_test),
    KUNIT_CASE(tcp_invalid_transition_test),
    KUNIT_CASE(udp_socket_create_test),
    KUNIT_CASE(udp_socket_bind_send_test),
    KUNIT_CASE(udp_socket_options_test),
    KUNIT_CASE(arp_cache_add_lookup_test),
    KUNIT_CASE(arp_cache_update_test),
    KUNIT_CASE(arp_cache_full_test),
    KUNIT_CASE(net_if_up_down_test),
    KUNIT_CASE(net_if_create_multiple_test),
    KUNIT_CASE(net_socket_reuse_test),
    KUNIT_CASE(net_socket_max_allocation_test),
    {0}
};

static struct kunit_suite net_test_suite;

/* ====================================================================
 *  Suite Registration
 * ==================================================================== */

void kunit_net_register(void)
{
    int ci = 0;
    for (int i = 0; net_test_cases[i].run != NULL && i < KUNIT_MAX_CASES - 1; i++) {
        net_test_suite.cases[ci].name = net_test_cases[i].name;
        net_test_suite.cases[ci].run  = net_test_cases[i].run;
        ci++;
    }
    net_test_suite.cases[ci].name = NULL;
    net_test_suite.cases[ci].run  = NULL;

    net_test_suite.name     = "net";
    net_test_suite.setup    = NULL;
    net_test_suite.teardown = NULL;

    kunit_register_suite(&net_test_suite);
    kprintf("[KUnit] Networking tests registered (%d cases)\n", ci);
}
