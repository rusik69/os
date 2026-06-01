/* wireguard.c — WireGuard-like cryptographic tunnel (stub) */

#define KERNEL_INTERNAL
#include "wireguard.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "rng.h"

static struct wg_device g_wg;
static int wg_initialized = 0;

int wg_init(void) {
    memset(&g_wg, 0, sizeof(g_wg));
    g_wg.listen_port = 51820;

    /* Generate random dummy key pair */
    for (int i = 0; i < 32; i++) {
        g_wg.private_key[i] = (uint8_t)(rng_get_u64() & 0xFF);
        g_wg.public_key[i] = g_wg.private_key[i] ^ 0xFF;
    }

    wg_initialized = 1;
    kprintf("[OK] WireGuard initialized (listen port %u)\\n", g_wg.listen_port);
    return 0;
}

int wg_create_peer(uint32_t endpoint_ip, uint16_t port) {
    if (!wg_initialized) return -1;
    if (g_wg.num_peers >= WG_MAX_PEERS) return -1;

    struct wg_peer *peer = &g_wg.peers[g_wg.num_peers];
    peer->endpoint_ip = endpoint_ip;
    peer->endpoint_port = port;
    peer->active = 1;

    /* Generate a random public key for the peer */
    for (int i = 0; i < 32; i++)
        peer->public_key[i] = (uint8_t)(rng_get_u64() & 0xFF);

    g_wg.num_peers++;
    kprintf("[WG] Added peer %d.%d.%d.%d:%u\\n",
            (uint8_t)(endpoint_ip >> 24), (uint8_t)(endpoint_ip >> 16),
            (uint8_t)(endpoint_ip >> 8), (uint8_t)endpoint_ip,
            port);
    return 0;
}

int wg_remove_peer(int index) {
    if (!wg_initialized) return -1;
    if (index < 0 || index >= g_wg.num_peers) return -1;

    g_wg.peers[index].active = 0;
    for (int i = index; i < g_wg.num_peers - 1; i++)
        g_wg.peers[i] = g_wg.peers[i + 1];
    g_wg.num_peers--;
    return 0;
}

int wg_send(const uint8_t *data, int len) {
    if (!wg_initialized || !data) return -1;
    if (g_wg.num_peers == 0) return -1;

    /* Stub: just log the send and pass through */
    kprintf("[WG] Send %d bytes to peer %d.%d.%d.%d:%u\\n",
            len,
            (uint8_t)(g_wg.peers[0].endpoint_ip >> 24),
            (uint8_t)(g_wg.peers[0].endpoint_ip >> 16),
            (uint8_t)(g_wg.peers[0].endpoint_ip >> 8),
            (uint8_t)g_wg.peers[0].endpoint_ip,
            g_wg.peers[0].endpoint_port);
    return len;
}

int wg_receive(const uint8_t *data, int len) {
    if (!wg_initialized || !data) return -1;

    /* Stub: just log receipt and pass through */
    (void)data;
    kprintf("[WG] Receive %d bytes (pass-through)\\n", len);
    return len;
}
