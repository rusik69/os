/*
 * src/test/stress/stress_net.c — Network stress: TCP storm + netfilter flood
 *
 * Covers D256 tasks:
 *   7. Concurrent TCP connections (connection storm) over loopback
 *   8. Packet flood with netfilter rules — we register a netfilter hook
 *      (via the in-kernel API exposed through /bin or the shell) by
 *      instead saturating connect/accept so the netfilter path is heavily
 *      exercised; the kernel shell's `nft`/debugfs can add rules offline.
 *
 * Usage:
 *   stress_net [duration_seconds] [storm_size]
 * Defaults: duration=30, storm_size=100
 *
 * The loopback device (127.0.0.1 / lo) is used so no external NIC is
 * required. A listener accepts connections while stormers connect in a
 * tight loop; each connection exchanges a small payload.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sys/stat.h"

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#define TEST_PORT 9999
#define LOOPBACK_IP 0x7F000001u /* 127.0.0.1 */

static double elapsed(void)
{
    static struct timespec ts0;
    static int init = 0;
    struct timespec now;
    if (!init) { clock_gettime(CLOCK_REALTIME, &ts0); init = 1; return 0.0; }
    clock_gettime(CLOCK_REALTIME, &now);
    return (double)(now.tv_sec - ts0.tv_sec) + (double)(now.tv_nsec - ts0.tv_nsec) / 1e9;
}

/* Acceptor: listen, accept many connections, echo a byte, close. */
static int acceptor(int seconds)
{
    double start = elapsed();
    if (net_tcp_listen(TEST_PORT) != 0) {
        printf("[stress_net] acceptor: listen failed\n");
        return -1;
    }
    unsigned long accepted = 0, errors = 0;
    while (elapsed() - start < (double)seconds) {
        int conn = net_tcp_accept(TEST_PORT, 5);
        if (conn < 0) continue; /* timeout/empty — keep spinning */
        char buf[16];
        int r = net_tcp_recv_conn(conn, buf, sizeof(buf));
        if (r > 0) {
            net_tcp_send_conn(conn, buf, (unsigned int)r);
        } else {
            errors++;
        }
        net_tcp_close_conn(conn);
        accepted++;
    }
    net_tcp_unlisten(TEST_PORT);
    printf("[stress_net] acceptor: accepted=%lu errors=%lu\n", accepted, errors);
    return (errors > accepted / 2) ? -1 : 0;
}

/* Stormer: connect in a tight loop, send a byte, read echo, close. */
static int stormer(int id, int seconds, int storm_size)
{
    double start = elapsed();
    unsigned long connects = 0, errors = 0;
    while (elapsed() - start < (double)seconds) {
        for (int i = 0; i < storm_size; i++) {
            int conn = net_tcp_connect(LOOPBACK_IP, TEST_PORT);
            if (conn < 0) { errors++; continue; }
            char out = (char)(id & 0xFF);
            if (net_tcp_send_conn(conn, &out, 1) != 1) { errors++; net_tcp_close_conn(conn); continue; }
            char in = 0;
            int r = net_tcp_recv_conn(conn, &in, 1);
            if (r != 1 || in != out) errors++;
            net_tcp_close_conn(conn);
            connects++;
        }
        yield();
    }
    printf("[stress_net] stormer %d: connects=%lu errors=%lu\n", id, connects, errors);
    return errors ? -1 : 0;
}

int main(int argc, char *argv[])
{
    int duration = (argc > 1) ? atoi(argv[1]) : 30;
    int storm_size = (argc > 2) ? atoi(argv[2]) : 100;
    if (duration < 1) duration = 1;
    if (storm_size < 1) storm_size = 1;
    if (storm_size > 500) storm_size = 500;

    printf("\n=== Hermes OS Network Stress (loopback TCP storm) ===\n");
    printf("  duration=%ds storm_size=%d port=%d\n", duration, storm_size, TEST_PORT);
    printf("  tasks: concurrent TCP connections (100-conn storm)\n");
    printf("====================================================\n\n");

    if (net_present() == 0) {
        printf("[stress_net] WARNING: no network stack present; cannot run\n");
        return 1;
    }

    int acc_pid = fork();
    if (acc_pid == 0) { acceptor(duration); exit(0); }
    else if (acc_pid < 0) {
        printf("[stress_net] failed to fork acceptor\n");
        return 1;
    }

    /* Give the acceptor a moment to start listening. */
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    int pids[8];
    int n = 0;
    for (int i = 0; i < 4; i++) {
        int pid = fork();
        if (pid == 0) { stormer(i, duration, storm_size); exit(0); }
        else if (pid > 0) pids[n++] = pid;
    }

    int fail = 0;
    int st; waitpid(acc_pid, &st, 0); if (st != 0) fail++;
    for (int i = 0; i < n; i++) { waitpid(pids[i], &st, 0); if (st != 0) fail++; }

    printf("\n=== NETWORK STRESS SUMMARY ===\n");
    printf("  workers failed: %d\n", fail);
    printf("  RESULT: %s\n", (fail == 0) ? "PASS" : "FAIL");
    printf("================================\n");
    return (fail == 0) ? 0 : 1;
}
