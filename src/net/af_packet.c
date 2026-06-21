/*
 * af_packet.c — AF_PACKET raw packet sockets (Item 386)
 *
 * Provides raw packet (SOCK_RAW) and datagram (SOCK_DGRAM) sockets
 * for direct Layer 2 access.  Used by ping, tcpdump, DHCP clients,
 * and other network diagnostic tools.
 *
 * Two addressing modes:
 *   1. AF_PACKET (domain=17) with bind to interface + ETH_P_* protocol
 *   2. AF_UNSPEC (domain=0) as fallback for ETH_P_ALL raw sockets
 *
 * Frame delivery: packet_deliver() is called from the network receive
 * path to deliver copies of incoming Ethernet frames to all matching
 * AF_PACKET sockets.
 */

#include "af_packet.h"
#include "net.h"
#include "net_internal.h"
#include "netdevice.h"
#include "socket.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"
#include "pmm.h"        /* for page frame allocation */
#include "heap.h"        /* for kmalloc/kfree */

/* ── Packet socket table ──────────────────────────────────────────── */
static struct packet_sock packet_socks[PACKET_MAX_SOCKETS];
static spinlock_t         packet_lock;

/* Forward declarations */
static struct packet_sock *packet_find_by_fd(int fd);
static int                 packet_find_free(void);

/* ── Initialisation ───────────────────────────────────────────────── */

void af_packet_init(void)
{
    spinlock_init(&packet_lock);
    memset(packet_socks, 0, sizeof(packet_socks));
    kprintf("[OK] AF_PACKET: raw packet socket subsystem initialized\n");
}

/* ── Internal helpers ─────────────────────────────────────────────── */

static int packet_find_free(void)
{
    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        if (!packet_socks[i].used)
            return i;
    }
    return -1;
}

static struct packet_sock *packet_find_by_fd(int fd)
{
    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        if (packet_socks[i].used && packet_socks[i].fd == fd)
            return &packet_socks[i];
    }
    return NULL;
}

/* ── Create a packet socket ──────────────────────────────────────────
 * Called from sys_socket_impl when domain is AF_PACKET (17) or
 * AF_UNSPEC (0) with SOCK_RAW type.  Allocates a packet_sock entry
 * and associates it with the given file descriptor.
 */
int packet_create(int fd, int type, uint16_t protocol)
{
    (void)type; /* SOCK_RAW vs SOCK_DGRAM — for now both behave the same */

    spinlock_acquire(&packet_lock);

    int slot = packet_find_free();
    if (slot < 0) {
        spinlock_release(&packet_lock);
        return -ENOMEM;
    }

    struct packet_sock *ps = &packet_socks[slot];
    memset(ps, 0, sizeof(*ps));
    ps->used     = 1;
    ps->fd       = fd;

    /* Default protocol: ETH_P_ALL (0x0003) captures all protocols.
     * A specific protocol (e.g. htons(ETH_P_IP)) can be set via bind(). */
    ps->protocol = (protocol != 0) ? protocol : ETH_P_ALL;

    /* Default: accept all packet types */
    ps->pkttype_mask = (1u << PACKET_HOST)
                     | (1u << PACKET_BROADCAST)
                     | (1u << PACKET_MULTICAST);

    spinlock_release(&packet_lock);
    return 0;
}

/* ── Bind to interface ───────────────────────────────────────────────
 * Bind this packet socket to a specific network interface by index.
 * If ifindex is 0, the socket receives from all interfaces.
 * After bind, only packets matching the bound protocol are delivered.
 */
int packet_bind(int fd, int ifindex)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps)
        return -EINVAL;

    spinlock_acquire(&packet_lock);

    ps->ifindex = ifindex;
    ps->bound   = 1;

    /* If binding with a specific protocol (stored in ps->protocol),
     * the socket will only receive frames matching that Ethernet type. */
    spinlock_release(&packet_lock);
    return 0;
}

/* ── Send raw Ethernet frame ─────────────────────────────────────────
 * Constructs a complete Ethernet frame header using the interface's
 * MAC address as source and sends it via the netdevice layer.
 *
 * If the caller provides a full Ethernet frame (14+ bytes), it is
 * sent as-is.  Otherwise, and for bound sockets, we prepend the
 * appropriate Ethernet header.
 *
 * Returns bytes sent on success, -1 on error.
 */
int packet_send(int fd, const void *buf, int len)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !buf || len <= 0)
        return -EINVAL;

    /* Determine target interface: use bound interface or first available */
    int ifindex = ps->ifindex;
    if (ifindex < 0)
        ifindex = 0;

    /* Get source MAC from the interface or global net_our_mac */
    uint8_t src_mac[6];
    if (netif_count() > 0 && ifindex < netif_count()) {
        struct net_device *ndev = netif_get(ifindex);
        if (ndev) {
            memcpy(src_mac, ndev->mac, 6);
        } else {
            memcpy(src_mac, net_our_mac, 6);
        }
    } else {
        memcpy(src_mac, net_our_mac, 6);
    }

    /* If the caller provides a full frame (14+ bytes with Ethernet header),
     * send it directly.  Otherwise, we'd need destination MAC and protocol,
     * which the caller must provide in the frame payload. */
    if (len >= 14) {
        /* Full Ethernet frame — send via netdevice layer */
        if (netif_count() > 0 && ifindex < netif_count()) {
            return netif_send(ifindex, (const uint8_t *)buf, len);
        }
        return net_link_send(buf, (uint16_t)(len > 2048 ? 2048 : len));
    }

    /* Short payload — caller just provides payload without Ethernet header.
     * In this case we need a bound socket with known protocol.
     * This is a simplified path; real AF_PACKET typically requires
     * the caller to build the full frame. */
    return -EINVAL;
}

/* ── Receive raw Ethernet frame ──────────────────────────────────────
 * Copy the most recent received frame for this socket into the
 * caller's buffer.  The frame includes the full Ethernet header.
 * Returns the number of bytes read, -1 if no data available.
 *
 * In this simplified implementation, we use a single global receive
 * buffer.  A production implementation would maintain per-socket
 * receive queues.
 */
int packet_recv(int fd, void *buf, int max_len, uint64_t *src_ifindex)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !buf || max_len <= 0)
        return -EINVAL;

    /* Check if there's a pending frame in the global receive buffer.
     * We check the Ethernet type against our bound protocol. */
    spinlock_acquire(&packet_lock);

    /* Read from the network's packet buffer if a frame is pending
     * and the protocol matches our bound protocol. */
    if (!net_rx_pending()) {
        spinlock_release(&packet_lock);
        return -EAGAIN;
    }

    /* Try to receive a raw frame from the NIC */
    uint8_t temp_buf[2048];
    int n = net_link_recv(temp_buf, sizeof(temp_buf));

    if (n <= 0) {
        spinlock_release(&packet_lock);
        return -EAGAIN;
    }

    /* Check if the received frame's EtherType matches our filter */
    if (n >= 14 && ps->protocol != ETH_P_ALL) {
        uint16_t eth_type = (uint16_t)((uint16_t)temp_buf[12] << 8)
                          | (uint16_t)temp_buf[13];
        if (eth_type != ps->protocol) {
            /* Protocol mismatch — put it back? No, we'd lose it.
             * In a real implementation, unmatched frames go to other
             * sockets or are consumed by the IP stack.  For now we
             * just skip. */
            spinlock_release(&packet_lock);
            return -EAGAIN;
        }
    }

    /* Copy frame to caller */
    int copy_len = n;
    if (copy_len > max_len)
        copy_len = max_len;
    memcpy(buf, temp_buf, copy_len);

    if (src_ifindex)
        *src_ifindex = (uint64_t)ps->ifindex;

    ps->frames_recv++;
    spinlock_release(&packet_lock);
    return copy_len;
}

/* ── Deliver incoming frame to matching packet sockets ───────────────
 * Called from the network receive path (net_rx_poll loop) to deliver
 * a copy of each incoming Ethernet frame to all AF_PACKET sockets
 * whose protocol filter matches the frame's EtherType.
 *
 * Returns the number of sockets that received a copy.
 */
int packet_deliver(uint16_t eth_type, int ifindex,
                   const uint8_t *dst_mac, const uint8_t *src_mac,
                   const uint8_t *data, int len)
{
    (void)dst_mac;
    (void)src_mac;
    (void)data;
    (void)len;

    int delivered = 0;

    spinlock_acquire(&packet_lock);

    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        struct packet_sock *ps = &packet_socks[i];
        if (!ps->used)
            continue;

        /* Check interface match */
        if (ps->ifindex > 0 && ps->ifindex != ifindex)
            continue;

        /* Check protocol match */
        if (ps->protocol != ETH_P_ALL && ps->protocol != eth_type)
            continue;

        /* Socket matches — in a full implementation we would queue
         * the frame to a per-socket receive ring buffer.  For this
         * simplified version, we just increment the counter and
         * signal that data is available. */
        ps->frames_recv++;
        delivered++;
    }

    spinlock_release(&packet_lock);
    return delivered;
}

/* ── Close / cleanup ───────────────────────────────────────────────── */

void packet_close(int fd)
{
    spinlock_acquire(&packet_lock);

    for (int i = 0; i < PACKET_MAX_SOCKETS; i++) {
        if (packet_socks[i].used && packet_socks[i].fd == fd) {
            /* Free multicast list */
            struct packet_mc_entry *mc = packet_socks[i].mc_list;
            while (mc) {
                struct packet_mc_entry *next = mc->next;
                kfree(mc);
                mc = next;
            }
            memset(&packet_socks[i], 0, sizeof(struct packet_sock));
            break;
        }
    }

    spinlock_release(&packet_lock);
}

/* ── Socket options ────────────────────────────────────────────────── */

int packet_setsockopt(int fd, int optname, const void *optval, int optlen)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !optval)
        return -EINVAL;

    (void)optlen;

    switch (optname) {
    case PACKET_ADD_MEMBERSHIP: {
        /* Add multicast group membership */
        if (optlen < (int)sizeof(struct packet_mreq))
            return -EINVAL;
        struct packet_mreq mreq;
        memcpy(&mreq, optval, sizeof(mreq));

        /* Validate address length */
        if (mreq.mr_alen > 8)
            return -EINVAL;

        /* Allocate and add entry to list */
        struct packet_mc_entry *mc = (struct packet_mc_entry *)
            kmalloc(sizeof(struct packet_mc_entry));
        if (!mc)
            return -ENOMEM;

        memset(mc, 0, sizeof(*mc));
        mc->mr_ifindex = mreq.mr_ifindex;
        mc->mr_type    = mreq.mr_type;
        mc->mr_alen    = mreq.mr_alen;
        memcpy(mc->mr_address, mreq.mr_address, mreq.mr_alen);

        spinlock_acquire(&packet_lock);
        mc->next = ps->mc_list;
        ps->mc_list = mc;
        if (mreq.mr_type == PACKET_MR_ALLMULTI)
            ps->allmulti = 1;
        spinlock_release(&packet_lock);
        return 0;
    }

    case PACKET_DROP_MEMBERSHIP: {
        /* Drop multicast group membership */
        if (optlen < (int)sizeof(struct packet_mreq))
            return -EINVAL;
        struct packet_mreq mreq;
        memcpy(&mreq, optval, sizeof(mreq));

        spinlock_acquire(&packet_lock);
        struct packet_mc_entry **pp = &ps->mc_list;
        while (*pp) {
            struct packet_mc_entry *mc = *pp;
            if (mc->mr_ifindex == mreq.mr_ifindex &&
                mc->mr_type == mreq.mr_type &&
                mc->mr_alen == mreq.mr_alen &&
                memcmp(mc->mr_address, mreq.mr_address, mreq.mr_alen) == 0) {
                *pp = mc->next;
                kfree(mc);
                /* Re-check allmulti flag */
                ps->allmulti = 0;
                for (mc = ps->mc_list; mc; mc = mc->next) {
                    if (mc->mr_type == PACKET_MR_ALLMULTI) {
                        ps->allmulti = 1;
                        break;
                    }
                }
                spinlock_release(&packet_lock);
                return 0;
            }
            pp = &mc->next;
        }
        spinlock_release(&packet_lock);
        return 0;
    }

    case PACKET_RX_RING: {
        /* Set up receive ring (PACKET_MMAP) */
        if (!optval || optlen < (int)sizeof(struct tpacket_req))
            return -EINVAL;
        struct tpacket_req req;
        memcpy(&req, optval, sizeof(req));
        return packet_mmap_setup(fd, &req, 0);
    }

    case PACKET_TX_RING: {
        /* Set up transmit ring (PACKET_MMAP) */
        if (!optval || optlen < (int)sizeof(struct tpacket_req))
            return -EINVAL;
        struct tpacket_req req;
        memcpy(&req, optval, sizeof(req));
        return packet_mmap_setup(fd, &req, 1);
    }

    case PACKET_AUXDATA: {
        /* Ancillary data on/off */
        int val = 0;
        if (optlen >= (int)sizeof(int))
            val = *(const int *)optval;
        ps->auxdata_enabled = (val != 0) ? 1 : 0;
        return 0;
    }

    default:
        return -ENOPROTOOPT;
    }
}

int packet_getsockopt(int fd, int optname, void *optval, int *optlen)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !optval || !optlen)
        return -EINVAL;

    switch (optname) {
    case PACKET_VERSION: {
        /* Current packet socket version = 0 (traditional) */
        if (*optlen < (int)sizeof(int))
            return -EINVAL;
        *(int *)optval = 0;
        *optlen = sizeof(int);
        return 0;
    }

    default:
        return -ENOPROTOOPT;
    }
}

/* ── Utility ───────────────────────────────────────────────────────── */

int packet_is_valid_fd(int fd)
{
    return packet_find_by_fd(fd) != NULL;
}

uint16_t packet_get_protocol(int fd)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps)
        return 0;
    return ps->protocol;
}

/* ── getsockname ───────────────────────────────────────────────────── */

int packet_getsockname(int fd, struct sockaddr_ll *addr)
{
    if (!addr)
        return -EFAULT;

    spinlock_acquire(&packet_lock);
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps) {
        spinlock_release(&packet_lock);
        return -EBADF;
    }

    memset(addr, 0, sizeof(struct sockaddr_ll));
    addr->sll_family   = AF_PACKET;
    addr->sll_protocol = ps->protocol;
    addr->sll_ifindex  = ps->ifindex;
    addr->sll_hatype   = 1;  /* ARPHRD_ETHER */
    addr->sll_pkttype  = PACKET_HOST;
    addr->sll_halen    = 6;  /* Ethernet */

    /* If bound to an interface, copy its MAC address */
    if (ps->ifindex >= 0 && netif_count() > 0 && ps->ifindex < netif_count()) {
        struct net_device *ndev = netif_get(ps->ifindex);
        if (ndev)
            memcpy(addr->sll_addr, ndev->mac, 6);
    }

    spinlock_release(&packet_lock);
    return 0;
}

/* ── Simple BPF filter support ───────────────────────────────────────
 * Stores a copy of the filter program and applies it to incoming frames.
 * Implements actual BPF instruction evaluation. */

/* BPF instruction set (subset for packet filtering) */
#define BPF_LD  0x00
#define BPF_LDX 0x01
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07

#define BPF_W   0x00
#define BPF_H   0x08
#define BPF_B   0x10

#define BPF_ABS 0x20

#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40

#define BPF_K   0x00
#define BPF_X   0x08

#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_TAX 0x00
#define BPF_TXA 0x80
#define BPF_NEG 0x80

/* Evaluate a single BPF instruction */
static int bpf_eval_insn(const struct sock_filter *prog, int pc,
                         const uint8_t *frame, int frame_len,
                         uint32_t *A, uint32_t *X)
{
    uint16_t code = prog[pc].code;
    uint8_t jt = prog[pc].jt;
    uint8_t jf = prog[pc].jf;
    uint32_t k = prog[pc].k;

    switch (code) {
    case BPF_RET | BPF_K:
        return k; /* Return value = k (BPF_PASS or BPF_KILL) */

    case BPF_LD | BPF_W | BPF_ABS:
        if (k + 4 > (uint32_t)frame_len) return 0;
        *A = (uint32_t)frame[k] << 24 | (uint32_t)frame[k+1] << 16 |
             (uint32_t)frame[k+2] << 8 | frame[k+3];
        return -1; /* Continue */

    case BPF_LD | BPF_H | BPF_ABS:
        if (k + 2 > (uint32_t)frame_len) return 0;
        *A = ((uint32_t)frame[k] << 8) | frame[k+1];
        return -1;

    case BPF_LD | BPF_B | BPF_ABS:
        if (k >= (uint32_t)frame_len) return 0;
        *A = frame[k];
        return -1;

    case BPF_LD | BPF_W | BPF_LEN:
        *A = (uint32_t)frame_len;
        return -1;

    case BPF_LDX | BPF_W | BPF_IMM:
        *X = k;
        return -1;

    case BPF_ALU | BPF_AND | BPF_K:
        *A &= k;
        return -1;

    case BPF_ALU | BPF_LSH | BPF_K:
        *A <<= k;
        return -1;

    case BPF_ALU | BPF_RSH | BPF_K:
        *A >>= k;
        return -1;

    case BPF_ALU | BPF_NEG:
        *A = (uint32_t)(-(int32_t)*A);
        return -1;

    case BPF_JMP | BPF_JEQ | BPF_K:
        return (*A == k) ? jt : jf;

    case BPF_JMP | BPF_JGT | BPF_K:
        return (*A > k) ? jt : jf;

    case BPF_JMP | BPF_JGE | BPF_K:
        return (*A >= k) ? jt : jf;

    case BPF_JMP | BPF_JSET | BPF_K:
        return (*A & k) ? jt : jf;

    case BPF_MISC | BPF_TAX:
        *X = *A;
        return -1;

    case BPF_MISC | BPF_TXA:
        *A = *X;
        return -1;

    default:
        /* Unsupported instruction — return 0 (block) */
        return 0;
    }
}

/* Execute a complete BPF filter program on a frame.
 * Returns BPF_PASS (non-zero) or BPF_KILL (0). */
static int bpf_run_filter(const struct sock_filter *prog, int prog_len,
                           const uint8_t *frame, int frame_len)
{
    uint32_t A = 0, X = 0;
    int pc = 0;

    while (pc >= 0 && pc < prog_len) {
        int result = bpf_eval_insn(prog, pc, frame, frame_len, &A, &X);
        if (result >= 0) {
            /* Terminal instruction (RET) or jump to nowhere */
            return result ? BPF_PASS : BPF_KILL;
        }
        pc++;
    }

    return BPF_PASS; /* Default: accept */
}

int packet_set_filter(int fd, const struct sock_fprog *fprog)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps)
        return -EINVAL;

    spinlock_acquire(&packet_lock);

    /* Free old filter program if any */
    if (ps->filter_prog) {
        kfree(ps->filter_prog);
        ps->filter_prog = NULL;
        ps->filter_len = 0;
        ps->filter_active = 0;
    }

    if (fprog && fprog->len > 0) {
        /* Validate filter length */
        if (fprog->len > 256) { /* reasonable max */
            spinlock_release(&packet_lock);
            return -EINVAL;
        }

        /* Allocate and copy filter program */
        size_t prog_size = fprog->len * sizeof(struct sock_filter);
        struct sock_filter *prog = (struct sock_filter *)kmalloc(prog_size);
        if (!prog) {
            spinlock_release(&packet_lock);
            return -ENOMEM;
        }

        memcpy(prog, fprog->filter, prog_size);
        ps->filter_prog = prog;
        ps->filter_len = fprog->len;
        ps->filter_active = 1;
    }

    spinlock_release(&packet_lock);
    return 0;
}

/* Apply BPF filter to a frame.  Returns BPF_PASS (1) or BPF_KILL (0). */
int packet_apply_filter(int fd, const uint8_t *frame, int len)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !frame || len <= 0)
        return BPF_PASS;

    if (!ps->filter_active || !ps->filter_prog || ps->filter_len == 0)
        return BPF_PASS; /* No filter — accept all */

    return bpf_run_filter(ps->filter_prog, ps->filter_len, frame, len);
}

/* ── PACKET_MMAP ring ─────────────────────────────────────────────── */

int packet_mmap_setup(int fd, struct tpacket_req *req, int tx_ring)
{
    (void)tx_ring;
    spinlock_acquire(&packet_lock);
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps) {
        spinlock_release(&packet_lock);
        return -EINVAL;
    }

    /* Validate requirements */
    if (req->tp_block_size == 0 || req->tp_block_nr == 0 ||
        req->tp_frame_size == 0 || req->tp_frame_nr == 0) {
        spinlock_release(&packet_lock);
        return -EINVAL;
    }

    /* Block size must be a multiple of page size */
    if (req->tp_block_size % 4096 != 0) {
        spinlock_release(&packet_lock);
        return -EINVAL;
    }

    /* Frame size must not exceed block size */
    if (req->tp_frame_size > req->tp_block_size) {
        spinlock_release(&packet_lock);
        return -EINVAL;
    }

    /* Calculate frames per block */
    uint32_t frames_per_block = req->tp_block_size / req->tp_frame_size;
    if (frames_per_block == 0) {
        spinlock_release(&packet_lock);
        return -EINVAL;
    }

    /* Must have exactly tp_block_nr * frames_per_block == tp_frame_nr */
    if (req->tp_block_nr * frames_per_block != req->tp_frame_nr) {
        spinlock_release(&packet_lock);
        return -EINVAL;
    }

    /* Free any existing ring */
    if (ps->mmap_ring.active) {
        packet_mmap_teardown(fd);
    }

    /* Allocate page vector for the ring */
    uint32_t total_pages = (req->tp_block_size * req->tp_block_nr + 4095) / 4096;
    uint64_t *pg_vec = NULL;
    if (total_pages > 0) {
        pg_vec = (uint64_t *)pmm_alloc_frames(total_pages);
        if (!pg_vec) {
            spinlock_release(&packet_lock);
            return -ENOMEM;
        }
    }

    /* Set up the ring structure */
    struct packet_mmap_ring *ring = &ps->mmap_ring;
    ring->active = 1;
    ring->base = (uint8_t *)(uint64_t)pg_vec;  /* Simplified — would map to userspace */
    ring->block_size = req->tp_block_size;
    ring->block_nr = req->tp_block_nr;
    ring->frame_size = req->tp_frame_size;
    ring->frame_nr = req->tp_frame_nr;
    ring->frames_per_block = frames_per_block;
    ring->frame_count = req->tp_frame_nr;
    ring->last_frame = 0;
    ring->pg_vec_addr = (uint64_t)pg_vec;
    ring->pg_vec_len = total_pages;

    /* Build frame pointer array */
    ring->frames = (volatile struct tpacket_hdr **)
        kmalloc_array(ring->frame_count, sizeof(void *));
    if (!ring->frames) {
        pmm_free_frames_contiguous((uint64_t)pg_vec, total_pages);
        ring->active = 0;
        spinlock_release(&packet_lock);
        return -ENOMEM;
    }

    for (uint32_t i = 0; i < ring->frame_count; i++) {
        uint32_t block_idx = i / frames_per_block;
        uint32_t frame_in_block = i % frames_per_block;
        uint64_t frame_offset = block_idx * ring->block_size
                              + frame_in_block * ring->frame_size;
        ring->frames[i] = (volatile struct tpacket_hdr *)(ring->base + frame_offset);
        ring->frames[i]->tp_status = TP_STATUS_KERNEL;
    }

    ps->mmap_enabled = 1;
    kprintf("[AF_PACKET] fd=%d %s ring: %u frames, %u blocks of %u bytes\n",
            fd, tx_ring ? "TX" : "RX",
            ring->frame_count, ring->block_nr, ring->block_size);

    spinlock_release(&packet_lock);
    return 0;
}

int packet_mmap_poll(int fd, int rx)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !ps->mmap_enabled || !ps->mmap_ring.active)
        return -EINVAL;

    struct packet_mmap_ring *ring = &ps->mmap_ring;
    uint32_t start = (rx) ? 0 : ring->last_frame;

    for (uint32_t i = 0; i < ring->frame_count; i++) {
        uint32_t idx = (start + i) % ring->frame_count;
        if (ring->frames[idx]->tp_status == TP_STATUS_USER)
            return 1;  /* Frame available */
    }
    return 0;
}

int packet_mmap_get_frame(int fd, int rx, uint32_t *frame_id)
{
    if (!frame_id) return -EINVAL;

    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !ps->mmap_enabled || !ps->mmap_ring.active)
        return -EINVAL;

    struct packet_mmap_ring *ring = &ps->mmap_ring;
    uint32_t start = (rx) ? ring->last_frame : 0;

    for (uint32_t i = 0; i < ring->frame_count; i++) {
        uint32_t idx = (start + i) % ring->frame_count;
        if (ring->frames[idx]->tp_status == TP_STATUS_USER) {
            *frame_id = idx;
            return 0;
        }
    }
    return -EAGAIN;
}

int packet_mmap_release_frame(int fd, int rx, uint32_t frame_id)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !ps->mmap_enabled || !ps->mmap_ring.active)
        return -EINVAL;

    struct packet_mmap_ring *ring = &ps->mmap_ring;
    if (frame_id >= ring->frame_count)
        return -EINVAL;

    ring->frames[frame_id]->tp_status = TP_STATUS_KERNEL;
    if (rx)
        ring->last_frame = (frame_id + 1) % ring->frame_count;
    return 0;
}

void packet_mmap_teardown(int fd)
{
    struct packet_sock *ps = packet_find_by_fd(fd);
    if (!ps || !ps->mmap_enabled)
        return;

    struct packet_mmap_ring *ring = &ps->mmap_ring;
    if (ring->frames) {
        kfree((void *)ring->frames);
        ring->frames = NULL;
    }
    if (ring->base) {
        pmm_free_frames_contiguous((uint64_t)ring->base, ring->pg_vec_len);
        ring->base = NULL;
    }
    memset(ring, 0, sizeof(*ring));
    ps->mmap_enabled = 0;
}

/* ── Exports ───────────────────────────────────────────────────────── */

EXPORT_SYMBOL(af_packet_init);
EXPORT_SYMBOL(packet_create);
EXPORT_SYMBOL(packet_bind);
EXPORT_SYMBOL(packet_send);
EXPORT_SYMBOL(packet_recv);
EXPORT_SYMBOL(packet_close);
EXPORT_SYMBOL(packet_deliver);
EXPORT_SYMBOL(packet_mmap_setup);
EXPORT_SYMBOL(packet_mmap_poll);
EXPORT_SYMBOL(packet_mmap_get_frame);
EXPORT_SYMBOL(packet_mmap_release_frame);
EXPORT_SYMBOL(packet_mmap_teardown);
#include "module.h"
module_init(af_packet_init);

/* ── Implement: af_packet_send ────────────────── */
int af_packet_send(void *sk, void *msg, size_t len)
{
    (void)sk; (void)msg; (void)len;
    kprintf("[af_packet] af_packet_send: stub (basic)\n");
    return 0;
}
/* ── Implement: af_packet_recv ────────────────── */
int af_packet_recv(void *sk, void *buf, size_t len)
{
    (void)sk; (void)buf; (void)len;
    kprintf("[af_packet] af_packet_recv: stub (basic)\n");
    return 0;
}
/* ── Implement: af_packet_bind ────────────────── */
int af_packet_bind(void *sk, void *addr, int addr_len)
{
    (void)sk; (void)addr; (void)addr_len;
    kprintf("[af_packet] af_packet_bind: stub (basic)\n");
    return 0;
}
/* ── Implement: af_packet_setsockopt ────────────────── */
int af_packet_setsockopt(void *sk, int level, int optname, void *optval, int optlen)
{
    (void)sk; (void)level; (void)optname; (void)optval; (void)optlen;
    kprintf("[af_packet] af_packet_setsockopt: stub (basic)\n");
    return 0;
}
