/* tun.c — TUN/TAP virtual network device */

#define KERNEL_INTERNAL
#include "tun.h"
#include "printf.h"
#include "string.h"
#include "heap.h"

static struct tun_device g_tun_dev;
static int tun_initialized = 0;

int tun_init(void) {
    if (tun_initialized) return -1;
    memset(&g_tun_dev, 0, sizeof(g_tun_dev));
    tun_initialized = 1;
    kprintf("[OK] TUN/TAP driver initialized\\n");
    return 0;
}

int tun_open(int flags) {
    if (!tun_initialized) return -1;
    if (g_tun_dev.opened) return -1;

    g_tun_dev.flags = flags;
    g_tun_dev.opened = 1;
    g_tun_dev.ring_head = 0;
    g_tun_dev.ring_tail = 0;
    g_tun_dev.ring_count = 0;
    memset(g_tun_dev.ring_len, 0, sizeof(g_tun_dev.ring_len));
    /* Return a fake fd */
    return 1000;
}

int tun_write(int fd, const void *data, uint16_t len) {
    (void)fd;
    if (!tun_initialized || !g_tun_dev.opened) return -1;
    if (!data || len == 0 || len > TUN_PKT_MAX) return -1;
    if (g_tun_dev.ring_count >= TUN_RING_SIZE) return -1;

    int tail = g_tun_dev.ring_tail;
    memcpy(g_tun_dev.ring_buf[tail], data, len);
    g_tun_dev.ring_len[tail] = len;
    g_tun_dev.ring_tail = (tail + 1) % TUN_RING_SIZE;
    g_tun_dev.ring_count++;
    return len;
}

int tun_read(int fd, void *buf, uint16_t max_len) {
    (void)fd;
    if (!tun_initialized || !g_tun_dev.opened) return -1;
    if (!buf || max_len == 0) return -1;
    if (g_tun_dev.ring_count == 0) return 0;

    int head = g_tun_dev.ring_head;
    uint16_t len = g_tun_dev.ring_len[head];
    if (len > max_len) len = max_len;

    memcpy(buf, g_tun_dev.ring_buf[head], len);
    g_tun_dev.ring_head = (head + 1) % TUN_RING_SIZE;
    g_tun_dev.ring_count--;
    return len;
}

void tun_destroy(void) {
    if (!tun_initialized) return;
    memset(&g_tun_dev, 0, sizeof(g_tun_dev));
    tun_initialized = 0;
}

/* ── Implement: tun_stop ────────────────── */
int tun_stop(void *dev)
{
    if (!dev) {
        kprintf("[tun] tun_stop: NULL dev\n");
        return -EINVAL;
    }
    kprintf("[tun] tun_stop: dev=%p (stub)\n", dev);
    return -EOPNOTSUPP;
}
/* ── Implement: tun_xmit ────────────────── */
int tun_xmit(void *skb, void *dev)
{
    if (!skb || !dev) {
        kprintf("[tun] tun_xmit: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[tun] tun_xmit: skb=%p dev=%p (stub)\n", skb, dev);
    return -EOPNOTSUPP;
}
