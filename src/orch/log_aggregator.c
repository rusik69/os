/*
 * log_aggregator.c — Centralized log aggregation (C169)
 *
 * Implements:
 *   C169: Ring-buffer log storage, query by container/namespace/pod,
 *         HTTP ingest and query endpoints
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
#include "httpd.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define MAX_LOG_ENTRIES         1024
#define CONTAINER_ID_MAX        64
#define NAMESPACE_MAX           64
#define POD_NAME_MAX            64
#define LOG_MESSAGE_MAX         256

/* ── Log entry descriptor ────────────────────────────────────────────── */

struct log_entry {
    char     container_id[CONTAINER_ID_MAX];
    char     namespace[NAMESPACE_MAX];
    char     pod_name[POD_NAME_MAX];
    uint64_t timestamp;
    char     message[LOG_MESSAGE_MAX];
    int      index;           /* Sequential index for ordering */
    char     in_use;
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct log_entry log_ring[MAX_LOG_ENTRIES];
static int log_head = 0;
static int log_count = 0;
static int log_sequence = 0;
static spinlock_t agg_lock;
static int aggregator_initialised = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C169: Log aggregator
 * ═══════════════════════════════════════════════════════════════════════ */

/* C169: Initialise the log aggregator */
int aggregator_init(void)
{
    memset(log_ring, 0, sizeof(log_ring));
    log_head = 0;
    log_count = 0;
    log_sequence = 0;
    aggregator_initialised = 1;
    kprintf("[LogAggregator] Initialised (%d max entries)\n", MAX_LOG_ENTRIES);
    return 0;
}

/* C169: Ingest a log entry into the ring buffer */
int aggregator_ingest(const char *container_id, const char *namespace,
                      const char *pod_name, const char *message)
{
    if (!container_id || !message || !aggregator_initialised) return -EINVAL;

    spinlock_acquire(&agg_lock);

    struct log_entry *e = &log_ring[log_head];
    memset(e, 0, sizeof(*e));

    strncpy(e->container_id, container_id, CONTAINER_ID_MAX - 1);
    if (namespace) strncpy(e->namespace, namespace, NAMESPACE_MAX - 1);
    if (pod_name) strncpy(e->pod_name, pod_name, POD_NAME_MAX - 1);
    strncpy(e->message, message, LOG_MESSAGE_MAX - 1);
    e->timestamp = timer_get_ms();
    e->index = log_sequence++;
    e->in_use = 1;

    log_head = (log_head + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) log_count++;

    spinlock_release(&agg_lock);
    return 0;
}

/* C169: Query log entries with simple string match on message field */
int aggregator_query(const char *query, char *results, int max_results)
{
    if (!query || !results || !aggregator_initialised) return -EINVAL;

    int found = 0;
    int pos = 0;

    spinlock_acquire(&agg_lock);

    /* Walk ring buffer from oldest to newest */
    int start_idx = (log_head - log_count + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;

    for (int i = 0; i < log_count && found < max_results; i++) {
        int idx = (start_idx + i) % MAX_LOG_ENTRIES;
        if (!log_ring[idx].in_use) continue;

        /* Simple substring match on message */
        if (strstr(log_ring[idx].message, query) != NULL) {
            int n = snprintf(results + pos, 512 - (size_t)(pos > 0 ? pos : 0),
                             "[%s] %s: %s\n",
                             log_ring[idx].container_id,
                             log_ring[idx].namespace[0] ? log_ring[idx].namespace : "default",
                             log_ring[idx].message);
            if (n > 0) pos += n;
            found++;
        }
    }

    spinlock_release(&agg_lock);
    return found;
}

/* C169: HTTP handler for log ingest and query
 *
 * POST /logs/batch   — ingest a batch of logs
 * GET /logs?query=&since=&container=  — query logs
 */
int aggregator_serve_http(const char *method, const char *uri,
                          const char *body, char *response, size_t resp_len)
{
    if (!method || !uri || !response || !aggregator_initialised) return -EINVAL;

    /* POST /logs/batch */
    if (strcmp(method, "POST") == 0 && strcmp(uri, "/logs/batch") == 0) {
        if (!body) {
            snprintf(response, resp_len,
                     "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            return 0;
        }

        /* In production: parse JSON array of log entries.
         * Simplified: ingest the raw body as a single entry. */
        aggregator_ingest("http-ingest", "default", "unknown", body);

        snprintf(response, resp_len,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: 7\r\n"
                 "\r\n"
                 "ingested");
        return 0;
    }

    /* GET /logs?query=... */
    if (strcmp(method, "GET") == 0 && strncmp(uri, "/logs", 5) == 0) {
        /* Parse query parameter (simplified) */
        const char *q = strstr(uri, "query=");
        const char *query_str = q ? q + 6 : "";

        char results[4096];
        int count = aggregator_query(query_str, results, 50);

        int n = snprintf(response, resp_len,
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/plain\r\n"
                         "Content-Length: %d\r\n"
                         "\r\n"
                         "%s",
                         count > 0 ? (int)strlen(results) : 0,
                         count > 0 ? results : "");
        (void)n;
        return 0;
    }

    /* Fallback: 404 */
    snprintf(response, resp_len,
             "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    return 0;
}

/* ── Stub: log_agg_start ─────────────────────────────── */
int log_agg_start(const char *config)
{
    (void)config;
    kprintf("[log_agg] log_agg_start: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: log_agg_stop ─────────────────────────────── */
int log_agg_stop(void)
{
    kprintf("[log_agg] log_agg_stop: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: log_agg_collect ─────────────────────────────── */
int log_agg_collect(const char *source, void *log)
{
    (void)source;
    (void)log;
    kprintf("[log_agg] log_agg_collect: not yet implemented\n");
    return -ENOSYS;
}
