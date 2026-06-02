/*
 * test_socket.c — Host-side unit tests for socket operations
 *
 * Tests the core socket state machine, allocation/deallocation,
 * option handling, address management, and a simplified loopback
 * data transfer mechanism that mirrors the kernel's socket
 * implementation in src/net/socket.c.
 *
 * The test implements a simplified socket model (sock_state machine,
 * sock_alloc/sock_free, bind/listen/connect/accept, send/recv via
 * in-memory loopback pairs) so that the algorithmic correctness
 * can be verified independently on the host.
 *
 * Compile:
 *   gcc -Wall -Werror -g -O0 -o test_socket test_socket.c
 * Run:
 *   ./test_socket
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 *  Kernel-compatible socket data structures
 *
 *  Mirrors the design in src/include/socket.h and src/net/socket.c
 *  — socket state machine, per-socket struct, allocation table.
 * =================================================================== */

#define SOCK_MAX       32
#define SOCK_BUF_SIZE  65536

/* Socket domain / type constants */
#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

/* Protocol constants */
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

/* Socket state machine (mirrors enum sock_state in socket.h) */
enum sock_state {
    SOCK_STATE_FREE = 0,
    SOCK_STATE_CREATED,
    SOCK_STATE_BOUND,
    SOCK_STATE_LISTENING,
    SOCK_STATE_CONNECTING,
    SOCK_STATE_CONNECTED,
    SOCK_STATE_CLOSED,
};

/* Socket option constants */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define SO_BROADCAST    6
#define SO_RCVBUF       8
#define SO_SNDBUF       7
#define SO_PRIORITY     12
#define SO_TYPE         3
#define SO_ERROR        4
#define TCP_NODELAY     1

/* Socket address */
struct sockaddr_in {
    uint16_t        sin_family;  /* AF_INET */
    uint16_t        sin_port;    /* port in network byte order */
    uint32_t        sin_addr;    /* internet address */
    char            sin_zero[8];
};

/* ── Per-socket structure (mirrors struct socket in socket.h) ──── */
typedef struct {
    int             in_use;
    int             domain;      /* AF_INET, AF_UNIX */
    int             type;        /* SOCK_STREAM, SOCK_DGRAM */
    int             protocol;    /* IPPROTO_TCP, IPPROTO_UDP */
    enum sock_state state;

    /* Addressing */
    uint32_t        local_ip;
    uint16_t        local_port;
    uint32_t        remote_ip;
    uint16_t        remote_port;

    /* Socket options */
    int             reuseaddr;
    int             keepalive;
    int             rcvbuf;
    int             sndbuf;
    int             tcp_nodelay;
    int             broadcast;
    int             priority;

    /* Loopback pair endpoint */
    int             pair_id;      /* -1 = not paired, >=0 = connected pair */
    int             pair_end;     /* LOOPBACK_END_A or LOOPBACK_END_B */
} socket_t;

/* ── Global socket table ──────────────────────────────────────── */
static socket_t socket_table[SOCK_MAX];

/* ── Loopback pair infrastructure ───────────────────────────────
 * A "loopback pair" simulates a bidirectional connection (like TCP)
 * between two socket endpoints.  Each pair has two uni-directional
 * data channels:
 *
 *   a_to_b: data written by slot A (local end) → read by slot B (peer end)
 *   b_to_a: data written by slot B (peer end) → read by slot A (local end)
 *
 * Each socket that shares the pair knows its "end" role (0 = A, 1 = B)
 * so that send() and recv() correctly route through the right channel.
 * ────────────────────────────────────────────────────────────── */

#define LOOPBACK_MAX_PAIRS  16
#define LOOPBACK_BUF_SIZE   65536

/* A uni-directional byte buffer channel */
typedef struct {
    uint8_t data[LOOPBACK_BUF_SIZE];
    size_t  write_cursor;    /* next write position */
    size_t  read_cursor;     /* next read position */
} loopback_channel_t;

/* A bidirectional loopback pair = two channels, one per direction */
typedef struct {
    int in_use;
    loopback_channel_t a_to_b;   /* A writes → B reads */
    loopback_channel_t b_to_a;   /* B writes → A reads */
} loopback_pair_t;

static loopback_pair_t loopback_pairs[LOOPBACK_MAX_PAIRS];

/* Find a free loopback pair and initialize it */
static int loopback_pair_create(void) {
    for (int i = 0; i < LOOPBACK_MAX_PAIRS; i++) {
        if (!loopback_pairs[i].in_use) {
            memset(&loopback_pairs[i], 0, sizeof(loopback_pair_t));
            loopback_pairs[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* Write data into a channel.
 * Returns bytes written, or -1 on error. */
static int loopback_channel_write(loopback_channel_t *ch,
                                  const uint8_t *buf, size_t len) {
    if (len > LOOPBACK_BUF_SIZE - ch->write_cursor) {
        /* Buffer full -- simulate EAGAIN by returning 0 */
        return 0;
    }
    memcpy(ch->data + ch->write_cursor, buf, len);
    ch->write_cursor += len;
    return (int)len;
}

/* Read data from a channel.
 * Returns bytes read, 0 if empty, or -1 on error. */
static int loopback_channel_read(loopback_channel_t *ch,
                                 uint8_t *buf, size_t max_len) {
    size_t available = ch->write_cursor - ch->read_cursor;
    if (available == 0) return 0;

    size_t to_read = (available < max_len) ? available : max_len;
    memcpy(buf, ch->data + ch->read_cursor, to_read);
    ch->read_cursor += to_read;
    return (int)to_read;
}

/* Destroy a loopback pair */
static void loopback_pair_destroy(int pair_id) {
    if (pair_id >= 0 && pair_id < LOOPBACK_MAX_PAIRS) {
        loopback_pairs[pair_id].in_use = 0;
    }
}

/* Pair endpoints from the perspective of the local socket */
#define LOOPBACK_END_A  0   /* "local" end of the pair (the creator / connect-er) */
#define LOOPBACK_END_B  1   /* "peer" end of the pair (the acceptor) */

/* ── Pending connection queue ───────────────────────────────────
 * When a client calls sock_connect(), a pending connection entry is
 * created with a fresh loopback pair.  When the server calls
 * sock_accept() on the matching listening socket, it dequeues one
 * pending entry and creates a new socket that shares the same pair
 * (as LOOPBACK_END_B), so that client and server can communicate
 * bidirectionally through the two channels of the shared pair.
 * ────────────────────────────────────────────────────────────── */

#define PENDING_CONN_MAX 16

typedef struct {
    int in_use;
    int pair_id;            /* the loopback pair for this connection */
    uint16_t local_port;    /* port the connecting socket bound to */
    uint16_t remote_port;   /* destination port (listener's port) */
    uint32_t remote_ip;     /* destination IP */
} pending_conn_t;

static pending_conn_t pending_conns[PENDING_CONN_MAX];

/* Enqueue a pending connection */
static int pending_conn_enqueue(int pair_id, uint16_t local_port,
                                uint16_t remote_port, uint32_t remote_ip) {
    for (int i = 0; i < PENDING_CONN_MAX; i++) {
        if (!pending_conns[i].in_use) {
            pending_conns[i].in_use = 1;
            pending_conns[i].pair_id = pair_id;
            pending_conns[i].local_port = local_port;
            pending_conns[i].remote_port = remote_port;
            pending_conns[i].remote_ip = remote_ip;
            return 0;
        }
    }
    return -1;  /* queue full */
}

/* Dequeue a pending connection for the given listening port.
 * Returns the pair_id, or -1 if no pending connection. */
static int pending_conn_dequeue(uint16_t listen_port) {
    for (int i = 0; i < PENDING_CONN_MAX; i++) {
        if (pending_conns[i].in_use &&
            pending_conns[i].remote_port == listen_port) {
            int pair_id = pending_conns[i].pair_id;
            pending_conns[i].in_use = 0;
            return pair_id;
        }
    }
    return -1;
}

/* ===================================================================
 *  Core socket operations (mirroring kernel sock_alloc / sock_free)
 * =================================================================== */

/* Initialize the socket table */
static void sock_test_init(void) {
    memset(socket_table, 0, sizeof(socket_table));
    /* Also reset loopback pairs */
    memset(loopback_pairs, 0, sizeof(loopback_pairs));
    /* Also reset pending connections */
    memset(pending_conns, 0, sizeof(pending_conns));
}

/* Allocate a socket slot.  Returns slot index (0..SOCK_MAX-1) or -1. */
static int sock_alloc(void) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (!socket_table[i].in_use) {
            memset(&socket_table[i], 0, sizeof(socket_t));
            socket_table[i].in_use = 1;
            socket_table[i].state = SOCK_STATE_CREATED;
            socket_table[i].pair_id = -1;
            socket_table[i].pair_end = -1;
            return i;
        }
    }
    return -1;
}

/* Free a socket. If it has a loopback pair, destroy it.
 * Also clean up any pending connection for its local port. */
static void sock_free(int slot) {
    if (slot < 0 || slot >= SOCK_MAX) return;
    if (!socket_table[slot].in_use) return;

    /* If this socket was bound and had pending connections, clean them */
    if (socket_table[slot].state >= SOCK_STATE_BOUND &&
        socket_table[slot].local_port > 0) {
        for (int i = 0; i < PENDING_CONN_MAX; i++) {
            if (pending_conns[i].in_use &&
                pending_conns[i].remote_port == socket_table[slot].local_port) {
                loopback_pair_destroy(pending_conns[i].pair_id);
                pending_conns[i].in_use = 0;
            }
        }
    }

    /* Destroy associated loopback pair if any */
    if (socket_table[slot].pair_id >= 0) {
        loopback_pair_destroy(socket_table[slot].pair_id);
    }

    socket_table[slot].in_use = 0;
    socket_table[slot].state = SOCK_STATE_FREE;
}

/* Create a socket (mirrors sys_socket_impl) */
static int sock_create(int domain, int type, int protocol) {
    int slot = sock_alloc();
    if (slot < 0) return -1;

    socket_table[slot].domain = domain;
    socket_table[slot].type = type;
    socket_table[slot].protocol = protocol;
    socket_table[slot].rcvbuf = 65536;
    socket_table[slot].sndbuf = 65536;

    /* Map protocol defaults */
    if (protocol == 0) {
        socket_table[slot].protocol = (type == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
    }

    return slot;
}

/* Bind a socket to an address (mirrors sys_bind_impl) */
static int sock_bind(int slot, uint32_t ip, uint16_t port) {
    if (slot < 0 || slot >= SOCK_MAX) return -1;
    socket_t *s = &socket_table[slot];
    if (!s->in_use) return -1;
    if (s->state != SOCK_STATE_CREATED) return -1;

    s->local_ip = ip;
    s->local_port = port;
    s->state = SOCK_STATE_BOUND;
    return 0;
}

/* Listen on a bound socket (mirrors sys_listen_impl) */
static int sock_listen(int slot, int backlog) {
    if (slot < 0 || slot >= SOCK_MAX) return -1;
    socket_t *s = &socket_table[slot];
    if (!s->in_use) return -1;
    if (s->state != SOCK_STATE_BOUND) return -1;
    if (s->type != SOCK_STREAM) return -1;

    s->state = SOCK_STATE_LISTENING;
    (void)backlog;
    return 0;
}

/* Connect a socket to a remote address (mirrors sys_connect_impl).
 * For SOCK_STREAM sockets, this also creates a loopback pair to
 * simulate the connection. */
static int sock_connect(int slot, uint32_t remote_ip, uint16_t remote_port) {
    if (slot < 0 || slot >= SOCK_MAX) return -1;
    socket_t *s = &socket_table[slot];
    if (!s->in_use) return -1;
    if (s->state != SOCK_STATE_CREATED && s->state != SOCK_STATE_BOUND) return -1;

    s->remote_ip = remote_ip;
    s->remote_port = remote_port;

    if (s->type == SOCK_STREAM) {
        /* Create a loopback pair to simulate TCP connection */
        int pair_id = loopback_pair_create();
        if (pair_id < 0) return -1;
        s->pair_id = pair_id;
        s->pair_end = LOOPBACK_END_A;   /* this socket is the "local" end */
        s->state = SOCK_STATE_CONNECTED;

        /* Enqueue as a pending connection for the target port so that
         * sock_accept() on the listening socket can pick it up. */
        uint16_t local_port = s->local_port;
        if (local_port == 0) local_port = 49152; /* ephemeral fallback */
        if (pending_conn_enqueue(pair_id, local_port,
                                 s->remote_port, s->remote_ip) < 0) {
            loopback_pair_destroy(pair_id);
            s->pair_id = -1;
            s->state = SOCK_STATE_CREATED;
            return -1;
        }
    } else {
        /* UDP: just mark connected (no loopback needed) */
        s->state = SOCK_STATE_CONNECTED;
    }

    return 0;
}

/* Accept a connection on a listening socket.
 * For testing purposes, we pick up a pending connection that was
 * created by sock_connect() and share its loopback pair. */
static int sock_accept(int listen_slot) {
    if (listen_slot < 0 || listen_slot >= SOCK_MAX) return -1;
    socket_t *ls = &socket_table[listen_slot];
    if (!ls->in_use) return -1;
    if (ls->state != SOCK_STATE_LISTENING) return -1;

    /* Dequeue a pending connection for this listening port */
    int pair_id = pending_conn_dequeue(ls->local_port);
    if (pair_id < 0) return -1;  /* no pending connection */

    /* Allocate a new socket for the accepted connection */
    int new_slot = sock_alloc();
    if (new_slot < 0) {
        loopback_pair_destroy(pair_id);
        return -1;
    }

    socket_t *ns = &socket_table[new_slot];
    ns->domain = ls->domain;
    ns->type = ls->type;
    ns->protocol = ls->protocol;
    ns->state = SOCK_STATE_CONNECTED;
    ns->local_ip = ls->local_ip;
    ns->local_port = ls->local_port;
    ns->remote_ip = 0x7F000001; /* 127.0.0.1 */
    ns->remote_port = ls->local_port + 1;
    ns->pair_id = pair_id;
    ns->pair_end = LOOPBACK_END_B;  /* accepted socket is the "peer" end */
    ns->rcvbuf = 65536;
    ns->sndbuf = 65536;

    return new_slot;
}

/* Send data on a connected socket (loopback).
 * For LOOPBACK_END_A: data goes to a_to_b channel.
 * For LOOPBACK_END_B: data goes to b_to_a channel.
 * Returns bytes written, or -1 on error. */
static int sock_send(int slot, const uint8_t *buf, size_t len) {
    if (slot < 0 || slot >= SOCK_MAX) return -1;
    socket_t *s = &socket_table[slot];
    if (!s->in_use) return -1;
    if (s->state != SOCK_STATE_CONNECTED) return -1;
    if (s->pair_id < 0) return -1;  /* no loopback endpoint for UDP */

    loopback_pair_t *pair = &loopback_pairs[s->pair_id];
    loopback_channel_t *ch;

    if (s->pair_end == LOOPBACK_END_A) {
        ch = &pair->a_to_b;   /* A writes to a_to_b, B reads from a_to_b */
    } else {
        ch = &pair->b_to_a;   /* B writes to b_to_a, A reads from b_to_a */
    }

    return loopback_channel_write(ch, buf, len);
}

/* Receive data on a connected socket (loopback).
 * For LOOPBACK_END_A: reads from b_to_a channel.
 * For LOOPBACK_END_B: reads from a_to_b channel.
 * Returns bytes read, 0 if empty, or -1 on error. */
static int sock_recv(int slot, uint8_t *buf, size_t max_len) {
    if (slot < 0 || slot >= SOCK_MAX) return -1;
    socket_t *s = &socket_table[slot];
    if (!s->in_use) return -1;
    if (s->state != SOCK_STATE_CONNECTED) return -1;
    if (s->pair_id < 0) return 0;  /* no loopback for UDP — nothing to read */

    loopback_pair_t *pair = &loopback_pairs[s->pair_id];
    loopback_channel_t *ch;

    if (s->pair_end == LOOPBACK_END_A) {
        ch = &pair->b_to_a;   /* A reads what B wrote */
    } else {
        ch = &pair->a_to_b;   /* B reads what A wrote */
    }

    return loopback_channel_read(ch, buf, max_len);
}

/* Close a socket (mirrors sock_free + TCP close) */
static void sock_close(int slot) {
    sock_free(slot);
}

/* Set a socket option (mirrors setsockopt) */
static int sock_setsockopt(int slot, int level, int optname, const void *val, size_t len) {
    if (slot < 0 || slot >= SOCK_MAX) return -1;
    socket_t *s = &socket_table[slot];
    if (!s->in_use) return -1;
    if (level != SOL_SOCKET && level != 6) return -1;  /* SOL_SOCKET or SOL_TCP */
    if (!val || len < sizeof(int)) return -1;

    int ival = *(const int *)val;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: s->reuseaddr = ival; return 0;
        case SO_KEEPALIVE: s->keepalive = ival; return 0;
        case SO_BROADCAST: s->broadcast = ival; return 0;
        case SO_RCVBUF:    s->rcvbuf = ival;    return 0;
        case SO_SNDBUF:    s->sndbuf = ival;    return 0;
        case SO_PRIORITY:  s->priority = ival;  return 0;
        default:           return -1;   /* unknown option */
        }
    }
    /* SOL_TCP */
    if (optname == TCP_NODELAY) {
        s->tcp_nodelay = ival;
        return 0;
    }

    return -1;
}

/* Get a socket option (mirrors getsockopt) */
static int sock_getsockopt(int slot, int level, int optname, void *val, size_t *len) {
    if (slot < 0 || slot >= SOCK_MAX) return -1;
    socket_t *s = &socket_table[slot];
    if (!s->in_use) return -1;
    if (!val || !len || *len < sizeof(int)) return -1;

    int *ival = (int *)val;

    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: *ival = s->reuseaddr; *len = sizeof(int); return 0;
        case SO_KEEPALIVE: *ival = s->keepalive; *len = sizeof(int); return 0;
        case SO_BROADCAST: *ival = s->broadcast; *len = sizeof(int); return 0;
        case SO_RCVBUF:    *ival = s->rcvbuf;    *len = sizeof(int); return 0;
        case SO_SNDBUF:    *ival = s->sndbuf;    *len = sizeof(int); return 0;
        case SO_PRIORITY:  *ival = s->priority;  *len = sizeof(int); return 0;
        case SO_TYPE:      *ival = s->type;      *len = sizeof(int); return 0;
        case SO_ERROR:     *ival = 0;            *len = sizeof(int); return 0;
        default:           return -1;
        }
    }
    if (level == 6) {
        switch (optname) {
        case TCP_NODELAY:  *ival = s->tcp_nodelay; *len = sizeof(int); return 0;
        default:           return -1;
        }
    }

    return -1;
}


/* ===================================================================
 *  Test framework
 * =================================================================== */

static int tests_total = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do {                                          \
    tests_total++;                                                      \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);\
    } else {                                                            \
        tests_passed++;                                                 \
    }                                                                   \
} while (0)

static void test_start(const char *name) {
    printf("  TEST: %-55s ... ", name);
    fflush(stdout);
}

static void test_end(void) {
    printf("%s\n", tests_passed == tests_total ? "PASS" : "FAIL");
}

/* ===================================================================
 *  Test cases
 * =================================================================== */

/* ── Allocation and Deallocation ────────────────────────────────── */

static void test_socket_alloc_free(void) {
    test_start("socket allocation returns valid slot");

    sock_test_init();
    int s1 = sock_alloc();
    ASSERT(s1 >= 0 && s1 < SOCK_MAX, "first alloc returns valid slot");
    ASSERT(socket_table[s1].in_use == 1, "socket marked in_use");
    ASSERT(socket_table[s1].state == SOCK_STATE_CREATED, "initial state is CREATED");

    sock_free(s1);
    ASSERT(socket_table[s1].in_use == 0, "socket freed");
    ASSERT(socket_table[s1].state == SOCK_STATE_FREE, "state is FREE after free");

    test_end();
}

static void test_socket_alloc_exhaustion(void) {
    test_start("socket allocation fails when table is full");

    sock_test_init();
    int slots[SOCK_MAX];
    int count = 0;

    /* Allocate all slots */
    for (int i = 0; i < SOCK_MAX; i++) {
        int s = sock_alloc();
        if (s >= 0) slots[count++] = s;
    }
    ASSERT(count == SOCK_MAX, "all SOCK_MAX sockets allocated");

    /* Next allocation must fail */
    int s_fail = sock_alloc();
    ASSERT(s_fail == -1, "allocation fails when full");

    /* Free one, then allocate again */
    sock_free(slots[0]);
    int s_reuse = sock_alloc();
    ASSERT(s_reuse >= 0, "allocation succeeds after free");

    /* Cleanup */
    for (int i = 1; i < count; i++) {
        if (socket_table[slots[i]].in_use) sock_free(slots[i]);
    }
    if (s_reuse >= 0) sock_free(s_reuse);

    test_end();
}

static void test_socket_double_free(void) {
    test_start("double free is a no-op (no crash)");

    sock_test_init();
    int s = sock_alloc();
    ASSERT(s >= 0, "alloc succeeds");

    sock_free(s);
    sock_free(s);  /* second free should be harmless */
    ASSERT(socket_table[s].in_use == 0, "socket still free after double free");

    test_end();
}

/* ── Socket Creation ──────────────────────────────────────────── */

static void test_socket_create_tcp(void) {
    test_start("socket create TCP (SOCK_STREAM)");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    ASSERT(s >= 0, "TCP socket created");
    ASSERT(socket_table[s].domain == AF_INET, "domain is AF_INET");
    ASSERT(socket_table[s].type == SOCK_STREAM, "type is SOCK_STREAM");
    ASSERT(socket_table[s].protocol == IPPROTO_TCP, "protocol maps to TCP");
    ASSERT(socket_table[s].state == SOCK_STATE_CREATED, "state is CREATED");
    ASSERT(socket_table[s].rcvbuf == 65536, "default rcvbuf is 65536");
    ASSERT(socket_table[s].sndbuf == 65536, "default sndbuf is 65536");

    sock_free(s);
    test_end();
}

static void test_socket_create_udp(void) {
    test_start("socket create UDP (SOCK_DGRAM)");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_DGRAM, 0);
    ASSERT(s >= 0, "UDP socket created");
    ASSERT(socket_table[s].domain == AF_INET, "domain is AF_INET");
    ASSERT(socket_table[s].type == SOCK_DGRAM, "type is SOCK_DGRAM");
    ASSERT(socket_table[s].protocol == IPPROTO_UDP, "protocol maps to UDP");

    sock_free(s);
    test_end();
}

static void test_socket_create_invalid_domain(void) {
    test_start("socket create with unsupported domain still works (kernel allows AF_UNSPEC)");

    sock_test_init();
    int s = sock_create(0xFF, SOCK_STREAM, 0);  /* invalid domain */
    ASSERT(s >= 0, "socket created even with unusual domain");

    sock_free(s);
    test_end();
}

/* ── Socket State Machine ──────────────────────────────────────── */

static void test_state_transitions_valid(void) {
    test_start("valid state transitions: CREATED -> BOUND -> LISTENING");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    ASSERT(s >= 0, "socket created");

    /* CREATED -> BOUND */
    int r = sock_bind(s, 0x7F000001, 8080);
    ASSERT(r == 0, "bind succeeds");
    ASSERT(socket_table[s].state == SOCK_STATE_BOUND, "state is BOUND");
    ASSERT(socket_table[s].local_ip == 0x7F000001, "local IP set");
    ASSERT(socket_table[s].local_port == 8080, "local port set");

    /* BOUND -> LISTENING */
    r = sock_listen(s, 5);
    ASSERT(r == 0, "listen succeeds");
    ASSERT(socket_table[s].state == SOCK_STATE_LISTENING, "state is LISTENING");

    sock_free(s);
    test_end();
}

static void test_state_transitions_invalid(void) {
    test_start("invalid state transitions are rejected");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    ASSERT(s >= 0, "socket created");

    /* Cannot listen before bind */
    int r = sock_listen(s, 5);
    ASSERT(r == -1, "listen on CREATED socket fails");

    /* Cannot accept before listen */
    r = sock_accept(s);
    ASSERT(r == -1, "accept on CREATED socket fails");

    /* Bind -> good */
    r = sock_bind(s, 0x7F000001, 8080);
    ASSERT(r == 0, "bind succeeds");

    /* Cannot bind twice */
    r = sock_bind(s, 0x7F000002, 8081);
    ASSERT(r == -1, "second bind on BOUND socket fails");

    /* Listen -> good */
    r = sock_listen(s, 5);
    ASSERT(r == 0, "listen succeeds");

    /* Cannot bind after listen */
    r = sock_bind(s, 0x7F000001, 8080);
    ASSERT(r == -1, "bind on LISTENING socket fails");

    /* Cannot listen again */
    r = sock_listen(s, 10);
    ASSERT(r == -1, "second listen on LISTENING socket fails");

    sock_free(s);
    test_end();
}

static void test_connect_state_before_bind(void) {
    test_start("connect from CREATED state is valid");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    ASSERT(s >= 0, "socket created");

    int r = sock_connect(s, 0x7F000001, 8080);
    ASSERT(r == 0, "connect from CREATED succeeds");
    ASSERT(socket_table[s].state == SOCK_STATE_CONNECTED, "state is CONNECTED");

    sock_free(s);
    test_end();
}

static void test_connect_state_after_bind(void) {
    test_start("connect from BOUND state is valid");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    ASSERT(s >= 0, "socket created");

    sock_bind(s, 0x7F000001, 9090);
    int r = sock_connect(s, 0x7F000002, 8080);
    ASSERT(r == 0, "connect from BOUND succeeds");
    ASSERT(socket_table[s].state == SOCK_STATE_CONNECTED, "state is CONNECTED");
    ASSERT(socket_table[s].remote_ip == 0x7F000002, "remote IP set");
    ASSERT(socket_table[s].remote_port == 8080, "remote port set");

    sock_free(s);
    test_end();
}

/* ── TCP Connect / Accept / Loopback Data Transfer ──────────────── */

static void test_tcp_connect_creates_loopback(void) {
    test_start("TCP connect creates loopback pair");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(s, 0x7F000001, 8080);

    ASSERT(socket_table[s].pair_id >= 0, "loopback pair created");
    ASSERT(loopback_pairs[socket_table[s].pair_id].in_use == 1, "loopback pair in use");

    sock_free(s);
    test_end();
}

static void test_tcp_accept_good(void) {
    test_start("accept on LISTENING socket returns new connected socket");

    sock_test_init();
    int listener = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_bind(listener, 0x7F000001, 8080);
    sock_listen(listener, 5);

    /* Create a client connection to enqueue a pending connect */
    int client = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(client, 0x7F000001, 8080);

    /* Now accept should pick up the pending connection */
    int accepted = sock_accept(listener);
    ASSERT(accepted >= 0, "accept succeeds");
    ASSERT(accepted != listener, "accepted socket is different from listener");
    ASSERT(accepted != client, "accepted socket is different from client");
    ASSERT(socket_table[accepted].state == SOCK_STATE_CONNECTED, "accepted socket is CONNECTED");
    ASSERT(socket_table[accepted].type == SOCK_STREAM, "accepted socket inherits type");
    ASSERT(socket_table[accepted].domain == AF_INET, "accepted socket inherits domain");
    ASSERT(socket_table[accepted].pair_id >= 0, "accepted socket has loopback pair");

    /* Verify client and accepted share the same loopback pair */
    ASSERT(socket_table[client].pair_id == socket_table[accepted].pair_id,
           "client and accepted share same loopback pair");

    sock_free(accepted);
    sock_free(client);
    sock_free(listener);
    test_end();
}

static void test_tcp_loopback_send_recv(void) {
    test_start("TCP loopback send/recv data transfer");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(s, 0x7F000001, 8080);
    ASSERT(socket_table[s].pair_id >= 0, "loopback pair created");
    ASSERT(socket_table[s].pair_end == LOOPBACK_END_A, "connector is end A");

    /* Send data via the connected socket (end A → writes to a_to_b) */
    const char *test_data = "Hello, loopback!";
    size_t send_len = strlen(test_data);
    int sent = sock_send(s, (const uint8_t *)test_data, send_len);
    ASSERT(sent == (int)send_len, "all data sent");

    /* Read from the same socket's recv — this should read from b_to_a,
     * which is empty since we only wrote to a_to_b. So we instead
     * verify by writing from the other end (simulating a peer response). */

    /* Write a response into b_to_a (simulating peer sending data back) */
    loopback_channel_t *ch_ba = &loopback_pairs[socket_table[s].pair_id].b_to_a;
    const char *response = "Response from peer";
    int r = loopback_channel_write(ch_ba, (const uint8_t *)response, strlen(response));
    ASSERT(r == (int)strlen(response), "peer response written");

    /* Now sock_recv on 's' should read from b_to_a */
    uint8_t recv_buf[128];
    int received = sock_recv(s, recv_buf, sizeof(recv_buf));
    ASSERT(received == (int)strlen(response), "received response");
    ASSERT(memcmp(recv_buf, response, strlen(response)) == 0, "response matches");

    sock_free(s);
    test_end();
}

static void test_tcp_accept_loopback_send_recv(void) {
    test_start("TCP accept + loopback send/recv (client->server)");

    sock_test_init();
    int listener = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_bind(listener, 0x7F000001, 8080);
    sock_listen(listener, 5);

    /* Client connects first (enqueues pending connection) */
    int client = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(client, 0x7F000001, 8080);

    /* Now accept picks up the pending connection */
    int accepted = sock_accept(listener);
    ASSERT(accepted >= 0, "accept succeeds");
    ASSERT(socket_table[client].pair_id == socket_table[accepted].pair_id,
           "client and accepted share loopback pair");

    /* Write from client (end A → a_to_b), read from server (end B → a_to_b) */
    const char *msg = "Data from client";
    int sent = sock_send(client, (const uint8_t *)msg, strlen(msg));
    ASSERT(sent == (int)strlen(msg), "client sent data");

    uint8_t buf[128];
    int recvd = sock_recv(accepted, buf, sizeof(buf));
    ASSERT(recvd == (int)strlen(msg), "server received correct amount");
    ASSERT(memcmp(buf, msg, strlen(msg)) == 0, "server received correct data");

    sock_free(client);
    sock_free(accepted);
    sock_free(listener);
    test_end();
}

static void test_tcp_loopback_bidirectional(void) {
    test_start("TCP loopback bidirectional data transfer");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(s, 0x7F000001, 8080);
    int pair_id = socket_table[s].pair_id;
    ASSERT(pair_id >= 0, "loopback pair created");
    ASSERT(socket_table[s].pair_end == LOOPBACK_END_A, "connector is end A");

    /* Write from end A (a_to_b) via sock_send, and from end B (b_to_a)
     * via direct channel access (simulating what the accepted socket would do). */
    const char *msg_a = "Direction A_to_B";
    sock_send(s, (const uint8_t *)msg_a, strlen(msg_a));

    /* Write from b_to_a directly (simulating the accepted socket sending) */
    const char *msg_b = "Direction B_to_A";
    loopback_channel_write(&loopback_pairs[pair_id].b_to_a,
                           (const uint8_t *)msg_b, strlen(msg_b));

    /* End A reads from b_to_a — should get msg_b */
    uint8_t buf_a[128];
    int recv_a = sock_recv(s, buf_a, sizeof(buf_a));
    ASSERT(recv_a == (int)strlen(msg_b), "end A received from B");
    ASSERT(memcmp(buf_a, msg_b, strlen(msg_b)) == 0, "end A data matches msg_b");

    /* End B would read from a_to_b — verify direct access */
    uint8_t buf_b[128];
    int recv_b = loopback_channel_read(&loopback_pairs[pair_id].a_to_b,
                                       buf_b, sizeof(buf_b));
    ASSERT(recv_b == (int)strlen(msg_a), "end B received from A");
    ASSERT(memcmp(buf_b, msg_a, strlen(msg_a)) == 0, "end B data matches msg_a");

    sock_free(s);
    test_end();
}

static void test_send_on_non_connected_fails(void) {
    test_start("send on non-connected socket fails");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);

    int r = sock_send(s, (const uint8_t *)"data", 4);
    ASSERT(r == -1, "send on CREATED socket fails");

    sock_bind(s, 0x7F000001, 8080);
    r = sock_send(s, (const uint8_t *)"data", 4);
    ASSERT(r == -1, "send on BOUND socket fails");

    sock_free(s);
    test_end();
}

/* ── Socket Options ────────────────────────────────────────────── */

static void test_setsockopt_getsockopt(void) {
    test_start("set and get socket options");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    ASSERT(s >= 0, "socket created");

    int val, r;

    /* SO_REUSEADDR */
    val = 1;
    r = sock_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    ASSERT(r == 0, "set SO_REUSEADDR");
    val = 0;
    size_t optlen = sizeof(val);
    r = sock_getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, &optlen);
    ASSERT(r == 0, "get SO_REUSEADDR");
    ASSERT(val == 1, "SO_REUSEADDR == 1");

    /* SO_KEEPALIVE */
    val = 1;
    sock_setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
    optlen = sizeof(val); val = 0;
    sock_getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &val, &optlen);
    ASSERT(val == 1, "SO_KEEPALIVE == 1");

    /* SO_RCVBUF */
    val = 128 * 1024;
    sock_setsockopt(s, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val));
    optlen = sizeof(val); val = 0;
    sock_getsockopt(s, SOL_SOCKET, SO_RCVBUF, &val, &optlen);
    ASSERT(val == 128 * 1024, "SO_RCVBUF == 131072");

    /* SO_SNDBUF */
    val = 64 * 1024;
    sock_setsockopt(s, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val));
    optlen = sizeof(val); val = 0;
    sock_getsockopt(s, SOL_SOCKET, SO_SNDBUF, &val, &optlen);
    ASSERT(val == 64 * 1024, "SO_SNDBUF == 65536");

    /* SO_BROADCAST */
    val = 1;
    sock_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &val, sizeof(val));
    optlen = sizeof(val); val = 0;
    sock_getsockopt(s, SOL_SOCKET, SO_BROADCAST, &val, &optlen);
    ASSERT(val == 1, "SO_BROADCAST == 1");

    /* SO_PRIORITY */
    val = 6;
    sock_setsockopt(s, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val));
    optlen = sizeof(val); val = 0;
    sock_getsockopt(s, SOL_SOCKET, SO_PRIORITY, &val, &optlen);
    ASSERT(val == 6, "SO_PRIORITY == 6");

    /* SO_TYPE (read-only) */
    optlen = sizeof(val); val = 0;
    sock_getsockopt(s, SOL_SOCKET, SO_TYPE, &val, &optlen);
    ASSERT(val == SOCK_STREAM, "SO_TYPE returns SOCK_STREAM");

    /* SO_ERROR (always 0 in our model) */
    optlen = sizeof(val); val = -1;
    sock_getsockopt(s, SOL_SOCKET, SO_ERROR, &val, &optlen);
    ASSERT(val == 0, "SO_ERROR == 0");

    /* TCP_NODELAY */
    val = 1;
    r = sock_setsockopt(s, 6, TCP_NODELAY, &val, sizeof(val));
    ASSERT(r == 0, "set TCP_NODELAY");
    optlen = sizeof(val); val = 0;
    r = sock_getsockopt(s, 6, TCP_NODELAY, &val, &optlen);
    ASSERT(r == 0, "get TCP_NODELAY");
    ASSERT(val == 1, "TCP_NODELAY == 1");

    sock_free(s);
    test_end();
}

static void test_setsockopt_invalid_optname(void) {
    test_start("setsockopt with unknown option name fails");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    int val = 1;

    int r = sock_setsockopt(s, SOL_SOCKET, 9999, &val, sizeof(val));
    ASSERT(r == -1, "unknown option returns -1");

    sock_free(s);
    test_end();
}

static void test_getsockopt_invalid_optname(void) {
    test_start("getsockopt with unknown option name fails");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    int val;
    size_t optlen = sizeof(val);

    int r = sock_getsockopt(s, SOL_SOCKET, 9999, &val, &optlen);
    ASSERT(r == -1, "unknown option returns -1");

    sock_free(s);
    test_end();
}

static void test_setsockopt_null_val(void) {
    test_start("setsockopt with NULL val fails");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    int r = sock_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, NULL, sizeof(int));
    ASSERT(r == -1, "NULL val returns -1");
    sock_free(s);
    test_end();
}

/* ── Edge Cases ────────────────────────────────────────────────── */

static void test_operations_on_freed_socket(void) {
    test_start("operations on freed socket behave correctly");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_free(s);

    int r = sock_bind(s, 0x7F000001, 8080);
    ASSERT(r == -1, "bind on freed socket fails");

    r = sock_listen(s, 5);
    ASSERT(r == -1, "listen on freed socket fails");

    r = sock_connect(s, 0x7F000001, 8080);
    ASSERT(r == -1, "connect on freed socket fails");

    r = sock_send(s, (const uint8_t *)"x", 1);
    ASSERT(r == -1, "send on freed socket fails");

    test_end();
}

static void test_close_removes_loopback_pair(void) {
    test_start("close destroys associated loopback pair");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(s, 0x7F000001, 8080);
    int pair_id = socket_table[s].pair_id;
    ASSERT(loopback_pairs[pair_id].in_use == 1, "pair active");

    sock_close(s);
    ASSERT(loopback_pairs[pair_id].in_use == 0, "pair destroyed after close");

    test_end();
}

static void test_large_data_transfer(void) {
    test_start("large data transfer through loopback");

    sock_test_init();

    /* Set up a listener */
    int listener = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_bind(listener, 0x7F000001, 8080);
    sock_listen(listener, 5);

    /* Client connects first (enqueues pending connection) */
    int client = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(client, 0x7F000001, 8080);

    /* Accept picks up the pending connection */
    int accepted = sock_accept(listener);
    ASSERT(accepted >= 0, "accept succeeds");

    /* Send a large buffer from client to server */
    size_t big_size = 10240;
    uint8_t *big_buf = (uint8_t *)malloc(big_size);
    ASSERT(big_buf != NULL, "large buffer allocated");

    for (size_t i = 0; i < big_size; i++) {
        big_buf[i] = (uint8_t)(i & 0xFF);
    }

    /* Client sends data → a_to_b */
    int sent = sock_send(client, big_buf, big_size);
    ASSERT(sent == (int)big_size, "large data sent");

    /* Server receives data → reads a_to_b */
    uint8_t *recv_buf = (uint8_t *)malloc(big_size);
    ASSERT(recv_buf != NULL, "receive buffer allocated");

    int received = sock_recv(accepted, recv_buf, big_size);
    ASSERT(received == (int)big_size, "large data received");
    ASSERT(memcmp(big_buf, recv_buf, big_size) == 0, "large data matches");

    free(big_buf);
    free(recv_buf);
    sock_free(client);
    sock_free(accepted);
    sock_free(listener);
    test_end();
}

static void test_empty_send(void) {
    test_start("send of 0 bytes returns 0");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(s, 0x7F000001, 8080);

    int r = sock_send(s, (const uint8_t *)"", 0);
    ASSERT(r == 0, "zero-length send returns 0");

    sock_free(s);
    test_end();
}

static void test_recv_from_empty_buffer(void) {
    test_start("recv from empty buffer returns 0");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_STREAM, 0);
    sock_connect(s, 0x7F000001, 8080);

    uint8_t buf[16];
    int r = sock_recv(s, buf, sizeof(buf));
    ASSERT(r == 0, "recv from empty returns 0");

    sock_free(s);
    test_end();
}

static void test_udp_connect_no_loopback(void) {
    test_start("UDP connect does not create a loopback pair");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_DGRAM, 0);
    int r = sock_connect(s, 0x7F000001, 8080);
    ASSERT(r == 0, "UDP connect succeeds");
    ASSERT(socket_table[s].state == SOCK_STATE_CONNECTED, "state is CONNECTED");
    ASSERT(socket_table[s].pair_id == -1, "no loopback pair for UDP");

    /* Send on UDP without loopback should fail (no pair) */
    r = sock_send(s, (const uint8_t *)"data", 4);
    ASSERT(r == -1, "UDP send with no pair returns -1");

    sock_free(s);
    test_end();
}

static void test_dgram_listen_not_stream(void) {
    test_start("listen on DGRAM socket fails");

    sock_test_init();
    int s = sock_create(AF_INET, SOCK_DGRAM, 0);
    sock_bind(s, 0x7F000001, 8080);

    int r = sock_listen(s, 5);
    ASSERT(r == -1, "listen on DGRAM socket fails");

    sock_free(s);
    test_end();
}

/* ── Cleanup / Leak Detection ──────────────────────────────────── */

static void test_all_sockets_freed_no_leaks(void) {
    test_start("all sockets freed leaves clean state (no leaked pairs)");

    sock_test_init();

    /* Allocate and free several sockets with various operations */
    for (int i = 0; i < 10; i++) {
        int s = sock_create(AF_INET, SOCK_STREAM, 0);
        sock_bind(s, 0x7F000001, 8000 + i);
        sock_listen(s, 5);
        int a = sock_accept(s);
        if (a >= 0) sock_free(a);
        sock_free(s);
    }

    for (int i = 0; i < 10; i++) {
        int s = sock_create(AF_INET, SOCK_STREAM, 0);
        sock_connect(s, 0x7F000001, 9000 + i);
        sock_free(s);
    }

    /* Verify all sockets are free */
    int used_slots = 0;
    for (int i = 0; i < SOCK_MAX; i++) {
        if (socket_table[i].in_use) used_slots++;
    }
    ASSERT(used_slots == 0, "all socket slots are free");

    /* Verify all loopback pairs are destroyed */
    int used_pairs = 0;
    for (int i = 0; i < LOOPBACK_MAX_PAIRS; i++) {
        if (loopback_pairs[i].in_use) used_pairs++;
    }
    ASSERT(used_pairs == 0, "all loopback pairs are destroyed");

    test_end();
}

/* ===================================================================
 *  Test runner
 * =================================================================== */

int main(void) {
    printf("\n=== Socket Operations Unit Tests ===\n\n");

    /* Allocation and Deallocation */
    test_socket_alloc_free();
    test_socket_alloc_exhaustion();
    test_socket_double_free();

    /* Socket Creation */
    test_socket_create_tcp();
    test_socket_create_udp();
    test_socket_create_invalid_domain();

    /* Socket State Machine */
    test_state_transitions_valid();
    test_state_transitions_invalid();
    test_connect_state_before_bind();
    test_connect_state_after_bind();

    /* TCP Connect / Accept / Loopback Data Transfer */
    test_tcp_connect_creates_loopback();
    test_tcp_accept_good();
    test_tcp_loopback_send_recv();
    test_tcp_accept_loopback_send_recv();
    test_tcp_loopback_bidirectional();
    test_send_on_non_connected_fails();

    /* Socket Options */
    test_setsockopt_getsockopt();
    test_setsockopt_invalid_optname();
    test_getsockopt_invalid_optname();
    test_setsockopt_null_val();

    /* Edge Cases */
    test_operations_on_freed_socket();
    test_close_removes_loopback_pair();
    test_large_data_transfer();
    test_empty_send();
    test_recv_from_empty_buffer();
    test_udp_connect_no_loopback();
    test_dgram_listen_not_stream();

    /* Cleanup */
    test_all_sockets_freed_no_leaks();

    /* ── Summary ────────────────────────────────────────────────── */
    printf("\n─── Results ─────────────────────────────────────────────\n");
    printf("  Total: %d  Passed: %d  Failed: %d\n\n",
           tests_total, tests_passed, tests_total - tests_passed);

    return (tests_passed == tests_total) ? 0 : 1;
}
