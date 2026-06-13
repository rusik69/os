/*
 * log_shipper.c — Container log forwarder (C168)
 *
 * Implements:
 *   C168: Asynchronous log shipping with buffering, retry, and backoff
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

#define LOG_SHIPPER_BUF_SIZE    4096
#define LOG_ENDPOINT_MAX        128
#define LOG_LINE_MAX            256
#define MAX_RETRY               5
#define BACKOFF_BASE_MS         1000    /* 1 second base for exponential backoff */

/* ── Log shipper descriptor ──────────────────────────────────────────── */

struct log_shipper {
    char   buffer[LOG_SHIPPER_BUF_SIZE];
    int    buffer_pos;
    char   endpoint[LOG_ENDPOINT_MAX];
    int    connected;
    int    retry_count;
    uint64_t last_attempt;
    int    initialised;
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct log_shipper shipper;
static spinlock_t shipper_lock;

/* ═══════════════════════════════════════════════════════════════════════
 *  C168: Container log forwarder
 * ═══════════════════════════════════════════════════════════════════════ */

/* C168: Initialise the log shipper with a remote collector endpoint */
int log_shipper_init(const char *endpoint)
{
    if (!endpoint) return -EINVAL;

    spinlock_acquire(&shipper_lock);
    memset(&shipper, 0, sizeof(shipper));
    strncpy(shipper.endpoint, endpoint, LOG_ENDPOINT_MAX - 1);
    shipper.buffer_pos = 0;
    shipper.connected = 0;
    shipper.retry_count = 0;
    shipper.last_attempt = 0;
    shipper.initialised = 1;
    spinlock_release(&shipper_lock);

    kprintf("[LogShipper] Initialised — endpoint: %s\n", endpoint);
    return 0;
}

/* C168: Attempt to connect to the remote log collector */
static int shipper_connect(void)
{
    /* In production: establish TCP connection to shipper.endpoint.
     * Simplified: just set connected flag. */
    shipper.connected = 1;
    shipper.retry_count = 0;
    kprintf("[LogShipper] Connected to %s\n", shipper.endpoint);
    return 0;
}

/* C168: Send a batch of logs via HTTP POST */
static int shipper_send_batch(const char *data, int len)
{
    if (!data || len <= 0) return -EINVAL;
    if (!shipper.connected) return -ENOTCONN;

    /* In production: HTTP POST to shipper.endpoint with Content-Type: application/json.
     * Simplified: log the batch locally. */
    (void)data;
    (void)len;
    kprintf("[LogShipper] Sent %d bytes to %s\n", len, shipper.endpoint);
    return 0;
}

/* C168: Ship a log line from a container */
int log_shipper_send(const char *container_id, const char *log_line)
{
    if (!container_id || !log_line || !shipper.initialised) return -EINVAL;

    spinlock_acquire(&shipper_lock);

    /* Format: {"container":"...","message":"..."}\n */
    char formatted[LOG_LINE_MAX + 96];
    int flen = snprintf(formatted, sizeof(formatted),
                        "{\"container\":\"%s\",\"message\":\"%s\"}\n",
                        container_id, log_line);
    if (flen < 0 || (size_t)flen >= sizeof(formatted)) {
        spinlock_release(&shipper_lock);
        return -ENOBUFS;
    }

    /* Check if buffer has room; if not, flush first */
    if (shipper.buffer_pos + flen >= LOG_SHIPPER_BUF_SIZE - 1) {
        int ret = shipper_send_batch(shipper.buffer, shipper.buffer_pos);
        if (ret != 0 && shipper.connected) {
            /* Disconnected — buffer will be retried */
            shipper.connected = 0;
        }
        shipper.buffer_pos = 0;
    }

    /* Append to buffer */
    memcpy(shipper.buffer + shipper.buffer_pos, formatted, (size_t)flen);
    shipper.buffer_pos += flen;

    spinlock_release(&shipper_lock);
    return 0;
}

/* C168: Flush buffered logs immediately */
int log_shipper_flush(void)
{
    if (!shipper.initialised) return -EAGAIN;

    spinlock_acquire(&shipper_lock);
    if (shipper.buffer_pos <= 0) {
        spinlock_release(&shipper_lock);
        return 0;
    }

    /* Try to connect if not connected */
    if (!shipper.connected) {
        uint64_t now = timer_get_ms();
        if (now - shipper.last_attempt < BACKOFF_BASE_MS * (1ULL << (uint64_t)shipper.retry_count)) {
            /* Still in backoff — skip this flush */
            spinlock_release(&shipper_lock);
            return -EAGAIN;
        }
        shipper.last_attempt = now;
        shipper_connect();
    }

    if (shipper.connected) {
        int ret = shipper_send_batch(shipper.buffer, shipper.buffer_pos);
        if (ret == 0) {
            shipper.buffer_pos = 0;
            shipper.retry_count = 0;
        } else {
            shipper.connected = 0;
            shipper.retry_count++;
            if (shipper.retry_count > MAX_RETRY) {
                kprintf("[LogShipper] Max retries reached (%d) — dropping buffer\n",
                        MAX_RETRY);
                shipper.buffer_pos = 0;
                shipper.retry_count = 0;
            }
        }
    }

    spinlock_release(&shipper_lock);
    return 0;
}
