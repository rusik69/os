/*
 * tracing.c — OpenTelemetry-style distributed tracing (C172, C173)
 *
 * Implements:
 *   C172: Trace span creation, lifecycle, and parent-child relationships
 *   C173: Span export in Jaeger/Zipkin-compatible format
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

#define TRACE_ID_MAX            32
#define SPAN_ID_MAX             32
#define SPAN_NAME_MAX           64
#define SPAN_STATUS_MAX         16
#define MAX_TRACE_SPANS         128
#define TRACE_BUF_SIZE          8192

/* Status values */
#define SPAN_STATUS_UNSET       0
#define SPAN_STATUS_OK          1
#define SPAN_STATUS_ERROR       2

/* ── Trace span descriptor ───────────────────────────────────────────── */

struct trace_span {
    char     trace_id[TRACE_ID_MAX];
    char     span_id[SPAN_ID_MAX];
    char     parent_span_id[SPAN_ID_MAX];
    char     name[SPAN_NAME_MAX];
    uint64_t start_ns;
    uint64_t end_ns;
    char     status[SPAN_STATUS_MAX];
    char     in_use;
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct trace_span trace_spans[MAX_TRACE_SPANS];
static int span_count = 0;
static int span_sequence = 0;
static spinlock_t trace_lock;
static int tracing_initialised = 0;

/* ── Span ID generation helper ──────────────────────────────────────── */

static void gen_hex_id(char *buf, size_t len)
{
    if (!buf || len < 2) return;
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len - 1; i++) {
        buf[i] = hex[(unsigned int)(timer_get_ms() + (uint64_t)i * 17) % 16];
    }
    buf[len - 1] = '\0';
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C172: Tracing — span creation and lifecycle
 * ═══════════════════════════════════════════════════════════════════════ */

/* C172: Initialise the tracing subsystem */
int tracing_init(void)
{
    memset(trace_spans, 0, sizeof(trace_spans));
    span_count = 0;
    span_sequence = 0;
    tracing_initialised = 1;
    kprintf("[Tracing] OpenTelemetry-style tracer initialised (max %d spans)\n",
            MAX_TRACE_SPANS);
    return 0;
}

/* C172: Start a new trace span */
int tracing_start_span(const char *name, const char *parent_span_id,
                       struct trace_span *out)
{
    if (!name || !out || !tracing_initialised) return -EINVAL;

    spinlock_acquire(&trace_lock);

    if (span_count >= MAX_TRACE_SPANS) {
        spinlock_release(&trace_lock);
        return -ENOSPC;
    }

    struct trace_span *s = &trace_spans[span_count++];
    memset(s, 0, sizeof(*s));

    /* Generate trace and span IDs */
    gen_hex_id(s->trace_id, TRACE_ID_MAX);
    snprintf(s->span_id, SPAN_ID_MAX, "%08x%08x",
             (unsigned int)(timer_get_ms()),
             (unsigned int)(++span_sequence));

    if (parent_span_id) {
        strncpy(s->parent_span_id, parent_span_id, SPAN_ID_MAX - 1);
    }

    strncpy(s->name, name, SPAN_NAME_MAX - 1);
    s->start_ns = timer_get_ms() * 1000000ULL; /* Convert ms → ns approximation */
    s->end_ns = 0;
    strncpy(s->status, "unset", SPAN_STATUS_MAX - 1);
    s->in_use = 1;

    memcpy(out, s, sizeof(*out));
    spinlock_release(&trace_lock);

    kprintf("[Tracing] Started span %s/%s (parent=%s)\n",
            s->trace_id, s->name, parent_span_id ? parent_span_id : "none");
    return 0;
}

/* C172: End a trace span (record end time, set status) */
int tracing_end_span(struct trace_span *span)
{
    if (!span || !tracing_initialised) return -EINVAL;

    spinlock_acquire(&trace_lock);

    span->end_ns = timer_get_ms() * 1000000ULL;
    strncpy(span->status, "ok", SPAN_STATUS_MAX - 1);

    /* Find and update in the global span table */
    for (int i = 0; i < MAX_TRACE_SPANS; i++) {
        if (!trace_spans[i].in_use) continue;
        if (strcmp(trace_spans[i].span_id, span->span_id) == 0 &&
            strcmp(trace_spans[i].trace_id, span->trace_id) == 0) {
            trace_spans[i].end_ns = span->end_ns;
            strncpy(trace_spans[i].status, "ok", SPAN_STATUS_MAX - 1);
            break;
        }
    }

    spinlock_release(&trace_lock);

    kprintf("[Tracing] Ended span %s/%s (duration=%lu ns)\n",
            span->trace_id, span->name,
            span->end_ns - span->start_ns);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  C173: Span export — Jaeger/Zipkin-compatible format
 * ═══════════════════════════════════════════════════════════════════════ */

/* C173: Export all completed spans in Jaeger-compatible batch format
 *
 * Outputs JSON-like span array:
 *   [{"traceId":"...","spanId":"...","parentSpanId":"...",
 *     "operationName":"...","startTime":...,"duration":...,"status":"..."}]
 */
int tracing_export(char *buf, size_t len)
{
    if (!buf || !tracing_initialised) return -EINVAL;

    int pos = 0;
    int first = 1;

    spinlock_acquire(&trace_lock);

    pos += snprintf(buf + pos, len - (size_t)pos, "[");

    for (int i = 0; i < MAX_TRACE_SPANS; i++) {
        if (!trace_spans[i].in_use) continue;
        if (trace_spans[i].end_ns == 0) continue; /* Skip incomplete spans */

        if (!first) {
            int n = snprintf(buf + pos, len - (size_t)pos, ",");
            if (n < 0) break;
            pos += n;
        }
        first = 0;

        uint64_t duration = trace_spans[i].end_ns - trace_spans[i].start_ns;
        int n = snprintf(buf + pos, len - (size_t)pos,
            "{\"traceId\":\"%s\","
            "\"spanId\":\"%s\","
            "\"parentSpanId\":\"%s\","
            "\"operationName\":\"%s\","
            "\"startTime\":%lu,"
            "\"duration\":%lu,"
            "\"status\":\"%s\"}",
            trace_spans[i].trace_id,
            trace_spans[i].span_id,
            trace_spans[i].parent_span_id[0] ? trace_spans[i].parent_span_id : "0",
            trace_spans[i].name,
            trace_spans[i].start_ns,
            duration,
            trace_spans[i].status);
        if (n < 0) break;
        pos += n;

        if ((size_t)pos >= len) break;
    }

    pos += snprintf(buf + pos, len - (size_t)pos, "]");

    spinlock_release(&trace_lock);
    return pos;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: tracing_start ───────────────────────────── */
int tracing_start(void)
{
    kprintf("[Tracing] tracing_start: not yet implemented\n");
    return 0;
}
/* ── Stub: tracing_stop ────────────────────────────── */
int tracing_stop(void)
{
    kprintf("[Tracing] tracing_stop: not yet implemented\n");
    return 0;
}
/* ── Stub: tracing_snapshot ────────────────────────── */
int tracing_snapshot(char *buf, size_t len)
{
    (void)buf;
    (void)len;
    kprintf("[Tracing] tracing_snapshot: not yet implemented\n");
    return 0;
}
/* ── Stub: tracing_set_filter ──────────────────────── */
int tracing_set_filter(const char *filter)
{
    (void)filter;
    kprintf("[Tracing] tracing_set_filter: not yet implemented\n");
    return 0;
}
/* ── Stub: tracing_set_rate ────────────────────────── */
int tracing_set_rate(uint32_t rate_hz)
{
    (void)rate_hz;
    kprintf("[Tracing] tracing_set_rate: not yet implemented\n");
    return 0;
}
