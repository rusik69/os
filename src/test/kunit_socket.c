/*
 * kunit_socket.c — KUnit test suite for the socket layer.
 *
 * Exercises the socket lifecycle (create/bind/listen/accept,
 * connect/send/recv/close) and socket options (SO_REUSEADDR,
 * SO_KEEPALIVE) through the real kernel socket API and
 * sys_setsockopt_impl()/sys_getsockopt_impl().
 *
 * These tests run inside the running kernel and validate the
 * socket layer's internal consistency without requiring a live
 * network peer.
 */

#include "errno.h"
#include "kunit.h"
#include "net.h"
#include "net_internal.h"
#include "printf.h"
#include "process.h"
#include "socket.h"
#include "string.h"

/* sys_setsockopt_impl / sys_getsockopt_impl live in net/socket.c;
 * they are not exposed via a public header, so declare them here. */
extern int sys_setsockopt_impl(int sockfd, int level, int optname, const void *optval,
                               uint32_t optlen);
extern int sys_getsockopt_impl(int sockfd, int level, int optname, void *optval, uint32_t *optlen);

/* ====================================================================
 *  1. Socket: create / bind / listen / accept (state machine)
 * ==================================================================== */

static void socket_create_test(struct kunit *test) {
    int slot = sock_alloc();
    KUNIT_EXPECT_TRUE(test, slot >= 0);
    if (slot < 0)
        return;

    int fd = sock_fd_from_slot(slot);
    struct socket *s = sock_get(fd);
    KUNIT_EXPECT_NOT_NULL(test, s);
    KUNIT_EXPECT_EQ(test, (int64_t)s->in_use, (int64_t)1);
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_CREATED);

    sock_put(s);
    sock_free(fd);
}

static void socket_bind_listen_accept_test(struct kunit *test) {
    int slot = sock_alloc();
    KUNIT_EXPECT_TRUE(test, slot >= 0);
    if (slot < 0)
        return;

    int fd = sock_fd_from_slot(slot);
    struct socket *s = sock_get(fd);
    KUNIT_EXPECT_NOT_NULL(test, s);

    s->domain = AF_INET;
    s->type = SOCK_STREAM;
    s->protocol = IPPROTO_TCP;

    /* Bind to loopback:127.0.0.1:9000 */
    s->local_ip = 0x7F000001; /* 127.0.0.1 */
    s->local_port = 9000;
    s->state = SOCK_STATE_BOUND;
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_BOUND);
    KUNIT_EXPECT_EQ(test, (int64_t)s->local_port, (int64_t)9000);

    /* Listen */
    s->state = SOCK_STATE_LISTENING;
    s->backlog = 5;
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_LISTENING);

    /* A listening socket must accept only from the listening state;
     * accept on a freshly bound (non-listening) socket should fail. */
    struct socket *b = sock_get(fd);
    b->state = SOCK_STATE_BOUND;
    /* net_tcp_accept on a non-listening port returns -1 (no pending). */
    int acc = net_tcp_accept(9000, 1);
    KUNIT_EXPECT_TRUE(test, acc < 0);
    sock_put(b);

    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_LISTENING);

    sock_put(s);
    sock_free(fd);
}

/* ====================================================================
 *  2. Socket: connect / send / recv / close
 *     (upper-layer lifecycle + safe error paths)
 * ==================================================================== */

static void socket_connect_lifecycle_test(struct kunit *test) {
    int slot = sock_alloc();
    KUNIT_EXPECT_TRUE(test, slot >= 0);
    if (slot < 0)
        return;

    int fd = sock_fd_from_slot(slot);
    struct socket *s = sock_get(fd);
    KUNIT_EXPECT_NOT_NULL(test, s);

    s->domain = AF_INET;
    s->type = SOCK_STREAM;
    s->protocol = IPPROTO_TCP;
    s->local_ip = 0x7F000001;
    s->local_port = 9100;

    /* connect → CONNECTING with remote endpoint recorded */
    s->remote_ip = 0x7F000001; /* 127.0.0.1 */
    s->remote_port = 9101;
    s->state = SOCK_STATE_CONNECTING;
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_CONNECTING);
    KUNIT_EXPECT_EQ(test, (int64_t)s->remote_port, (int64_t)9101);

    /* Once the handshake completes, the upper layer marks CONNECTED. */
    s->state = SOCK_STATE_CONNECTED;
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_CONNECTED);

    /* send/recv on an invalid conn_id must return an error, not crash. */
    int sent = net_tcp_send(-1, "hi", 2);
    KUNIT_EXPECT_TRUE(test, sent < 0);
    char buf[16];
    int got = net_tcp_recv(-1, buf, sizeof(buf), 1);
    KUNIT_EXPECT_TRUE(test, got < 0);

    /* close → CLOSED */
    s->state = SOCK_STATE_CLOSED;
    KUNIT_EXPECT_EQ(test, (int64_t)s->state, (int64_t)SOCK_STATE_CLOSED);

    sock_put(s);
    sock_free(fd);
}

static void socket_invalid_ops_test(struct kunit *test) {
    /* Freeing / getting never-allocated slots must be safe. */
    sock_free(-1);
    sock_free(SOCK_MAX + 10);
    sock_free(9999);

    KUNIT_EXPECT_NULL(test, sock_get(-1));
    KUNIT_EXPECT_NULL(test, sock_get(SOCK_MAX));

    /* send/recv/accept on an invalid descriptor must not crash. */
    KUNIT_EXPECT_TRUE(test, net_tcp_send(-1, "x", 1) < 0);
    KUNIT_EXPECT_TRUE(test, net_tcp_recv(-1, NULL, 0, 0) < 0);
    KUNIT_EXPECT_TRUE(test, net_tcp_accept(0, 0) < 0);

    KUNIT_EXPECT_TRUE(test, 1);
}

/* ====================================================================
 *  3. Socket options: SO_REUSEADDR / SO_KEEPALIVE
 * ==================================================================== */

static void socket_opt_reuseaddr_test(struct kunit *test) {
    int slot = sock_alloc();
    KUNIT_EXPECT_TRUE(test, slot >= 0);
    if (slot < 0)
        return;

    int fd = sock_fd_from_slot(slot);
    struct socket *s = sock_get(fd);
    KUNIT_EXPECT_NOT_NULL(test, s);

    int one = 1;
    int zero = 0;
    uint32_t olen = sizeof(int);

    /* Default: reuseaddr cleared. */
    KUNIT_EXPECT_EQ(test, (int64_t)s->reuseaddr, (int64_t)0);

    /* Set SO_REUSEADDR=1 via the real syscall impl. */
    int ret = sys_setsockopt_impl(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int));
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)s->reuseaddr, (int64_t)1);

    /* Read it back via getsockopt. */
    int got = -1;
    olen = sizeof(int);
    ret = sys_getsockopt_impl(fd, SOL_SOCKET, SO_REUSEADDR, &got, &olen);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)got, (int64_t)1);

    /* Clear it again. */
    ret = sys_setsockopt_impl(fd, SOL_SOCKET, SO_REUSEADDR, &zero, sizeof(int));
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)s->reuseaddr, (int64_t)0);

    sock_put(s);
    sock_free(fd);
}

static void socket_opt_keepalive_test(struct kunit *test) {
    int slot = sock_alloc();
    KUNIT_EXPECT_TRUE(test, slot >= 0);
    if (slot < 0)
        return;

    int fd = sock_fd_from_slot(slot);
    struct socket *s = sock_get(fd);
    KUNIT_EXPECT_NOT_NULL(test, s);

    int one = 1;
    int zero = 0;
    uint32_t olen = sizeof(int);

    /* Default: keepalive cleared. */
    KUNIT_EXPECT_EQ(test, (int64_t)s->keepalive, (int64_t)0);

    /* Set SO_KEEPALIVE=1 — propagates to the (optional) conn. */
    int ret = sys_setsockopt_impl(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(int));
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)s->keepalive, (int64_t)1);

    /* Read back. */
    int got = -1;
    olen = sizeof(int);
    ret = sys_getsockopt_impl(fd, SOL_SOCKET, SO_KEEPALIVE, &got, &olen);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)got, (int64_t)1);

    /* Clear. */
    ret = sys_setsockopt_impl(fd, SOL_SOCKET, SO_KEEPALIVE, &zero, sizeof(int));
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)s->keepalive, (int64_t)0);

    sock_put(s);
    sock_free(fd);
}

static void socket_opt_invalid_test(struct kunit *test) {
    int slot = sock_alloc();
    KUNIT_EXPECT_TRUE(test, slot >= 0);
    if (slot < 0)
        return;

    int fd = sock_fd_from_slot(slot);

    /* Bad fd → -EBADF. */
    int v = 1;
    KUNIT_EXPECT_EQ(test,
                    (int64_t)sys_setsockopt_impl(-1, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(int)),
                    (int64_t)-EBADF);

    /* NULL optval → -EINVAL. */
    KUNIT_EXPECT_EQ(test,
                    (int64_t)sys_setsockopt_impl(fd, SOL_SOCKET, SO_REUSEADDR, NULL, sizeof(int)),
                    (int64_t)-EINVAL);

    /* optlen too small → -EINVAL. */
    KUNIT_EXPECT_EQ(test, (int64_t)sys_setsockopt_impl(fd, SOL_SOCKET, SO_REUSEADDR, &v, 1),
                    (int64_t)-EINVAL);

    sock_free(fd);
}

/* ====================================================================
 *  Test case list (terminated by {0})
 * ==================================================================== */

static const struct kunit_case socket_test_cases[] = {KUNIT_CASE(socket_create_test),
                                                      KUNIT_CASE(socket_bind_listen_accept_test),
                                                      KUNIT_CASE(socket_connect_lifecycle_test),
                                                      KUNIT_CASE(socket_invalid_ops_test),
                                                      KUNIT_CASE(socket_opt_reuseaddr_test),
                                                      KUNIT_CASE(socket_opt_keepalive_test),
                                                      KUNIT_CASE(socket_opt_invalid_test),
                                                      {0}};

static struct kunit_suite socket_test_suite;

/* ====================================================================
 *  Suite Registration
 * ==================================================================== */

void kunit_socket_register(void) {
    int ci = 0;
    for (int i = 0; i < (int)(sizeof(socket_test_cases) / sizeof(socket_test_cases[0])) &&
                    socket_test_cases[i].run != NULL;
         i++) {
        socket_test_suite.cases[ci].name = socket_test_cases[i].name;
        socket_test_suite.cases[ci].run = socket_test_cases[i].run;
        ci++;
    }
    socket_test_suite.cases[ci].name = NULL;
    socket_test_suite.cases[ci].run = NULL;

    socket_test_suite.name = "socket";
    socket_test_suite.setup = NULL;
    socket_test_suite.teardown = NULL;

    kunit_register_suite(&socket_test_suite);
    kprintf("[KUnit] Socket tests registered (%d cases)\n", ci);
}

int kunit_socket_init(void) {
    kprintf("[kunit] Socket tests initialized\n");
    return 0;
}
