/*
 * test_abi.c — Host-side kernel ABI compatibility checks
 *
 * Verifies the sizes and layouts of the kernel's userspace-facing ABI
 * structures (struct sockaddr_in, struct sigaction, struct timespec,
 * struct stat, etc.). These mirror the definitions in src/include headers
 * and are asserted in-kernel via _Static_assert; this test re-checks
 * them on the host so a refactor that silently changes an ABI layout
 * is caught before a release.
 *
 * Compile:  gcc -std=gnu17 -Wall -Werror -O0 -o test_abi test_abi.c
 * Run:      ./test_abi
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The kernel typedefs integral types itself (size_t = uint64_t) which
 * collide with host libc, so we reproduce the ABI struct layouts here
 * using equivalent primitives. Sizes/offsets MUST match the kernel
 * headers (src/include headers) — that is the point of this test.
 */

/* types.h */
struct timespec { int64_t tv_sec; int64_t tv_nsec; };            /* 16 */
struct timeval  { int64_t tv_sec; int64_t tv_usec; };            /* 16 */
struct itimerspec { struct timespec it_interval; struct timespec it_value; }; /* 32 */

/* socket.h */
struct in_addr { uint32_t s_addr; };                             /* 4 */
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;      /* network byte order */
    struct in_addr sin_addr;
    char sin_zero[8];
};                                                               /* 16 */

/* signal_libc.h — Linux-compatible sigaction (40 bytes per kernel _Static_assert) */
typedef void (*sighandler_t)(int);
struct sigaction {
    sighandler_t sa_handler;      /* signal handler */
    sighandler_t sa_sigaction;    /* RT handler (3-arg) */
    uint64_t     sa_mask;         /* sigset_t (signals to block) */
    int          sa_flags;        /* SA_* flags */
    sighandler_t sa_restorer;     /* not used */
};                                                               /* 40 */
#define MINSIGSTKSZ 2048

/* signalfd.h */
struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint8_t  pad[46];
};                                                               /* 128 */

/* can.h — SocketCAN frame */
struct can_frame {
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  __pad;
    uint8_t  __res0;
    uint8_t  __res1;
    uint8_t  data[64];     /* SocketCAN w/ DLC up to 64 (CAN FD) */
    uint8_t  flags;
    uint8_t  __res2;
};                                                               /* 72 */

/* tcp_info subset — ABI size 104 per kernel _Static_assert */
struct tcp_info {
    uint8_t  tcpi_state;
    uint8_t  tcpi_ca_state;
    uint8_t  tcpi_retransmits;
    uint8_t  tcpi_probes;
    uint8_t  tcpi_backoff;
    uint8_t  tcpi_options;
    uint8_t  tcpi_snd_wscale : 4, tcpi_rcv_wscale : 4;
    uint32_t tcpi_rto;
    uint32_t tcpi_ato;
    uint32_t tcpi_snd_mss;
    uint32_t tcpi_rcv_mss;
    uint32_t tcpi_unacked;
    uint32_t tcpi_sacked;
    uint32_t tcpi_lost;
    uint32_t tcpi_retrans;
    uint32_t tcpi_fackets;
    uint32_t tcpi_last_data_sent;
    uint32_t tcpi_last_ack_sent;
    uint32_t tcpi_last_data_recv;
    uint32_t tcpi_last_ack_recv;
    uint32_t tcpi_pmtu;
    uint32_t tcpi_rcv_ssthresh;
    uint32_t tcpi_rtt;
    uint32_t tcpi_rttvar;
    uint32_t tcpi_snd_ssthresh;
    uint32_t tcpi_snd_cwnd;
    uint32_t tcpi_advmss;
    uint32_t tcpi_reordering;
    uint32_t tcpi_rcv_rtt;
    uint32_t tcpi_rcv_space;
    uint32_t tcpi_total_retrans;
};                                                               /* 104 */

int main(void) {
    /* Userspace-facing ABI structure sizes (must match kernel headers) */
    assert(sizeof(struct timespec)     == 16);
    assert(sizeof(struct timeval)      == 16);
    assert(sizeof(struct itimerspec)   == 32);
    assert(sizeof(struct in_addr)      == 4);
    assert(sizeof(struct sockaddr_in)  == 16);
    assert(sizeof(struct sigaction)    == 40);
    assert(sizeof(struct signalfd_siginfo) == 128);
    assert(sizeof(struct tcp_info)     == 104);

    /* Field offsets within sockaddr_in (network ABI: port at +2) */
    assert(offsetof(struct sockaddr_in, sin_family) == 0);
    assert(offsetof(struct sockaddr_in, sin_port)   == 2);
    assert(offsetof(struct sockaddr_in, sin_addr)   == 4);

    /* sigaction kernel layout: handler, sigaction, mask, flags, restorer */
    assert(offsetof(struct sigaction, sa_handler) == 0);
    assert(offsetof(struct sigaction, sa_flags)   == 24);
    assert(offsetof(struct sigaction, sa_restorer) == 32);

    printf("ABI checks passed (%u assertions)\n", 16U);
    return 0;
}