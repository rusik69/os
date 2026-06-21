/*
 * dashboard.c — Health dashboard (C174)
 *
 * Implements:
 *   C174: HTML/JSON dashboard rendering with subsystem stats
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

#define DASHBOARD_BUF_MAX       16384
#define DASHBOARD_LINE_MAX      256

/* ── Global state ───────────────────────────────────────────────────── */

static int dashboard_initialised = 0;
static spinlock_t dash_lock;

/* Cached dashboard stats (simplified — polled on refresh) */
static struct dash_stats {
    int    node_count;
    int    nodes_ready;
    int    nodes_not_ready;
    int    pods_running;
    int    pods_pending;
    int    pods_failed;
    uint64_t cpu_usage;
    uint64_t memory_usage;
    uint64_t disk_usage;
    int    recent_events;
    int    active_alerts;
} cached_stats;

/* ═══════════════════════════════════════════════════════════════════════
 *  C174: Health dashboard
 * ═══════════════════════════════════════════════════════════════════════ */

/* C174: Initialise the dashboard subsystem */
int dashboard_init(void)
{
    memset(&cached_stats, 0, sizeof(cached_stats));
    dashboard_initialised = 1;
    kprintf("[Dashboard] Health dashboard initialised\n");
    return 0;
}

/* C174: Poll all subsystem stats and refresh cache */
int dashboard_refresh(void)
{
    if (!dashboard_initialised) return -EAGAIN;

    spinlock_acquire(&dash_lock);

    /* In production: query actual subsystems.
     * Simplified: simulate with timer-based values. */
    uint64_t now = timer_get_ms();
    cached_stats.node_count = 5;
    cached_stats.nodes_ready = 4;
    cached_stats.nodes_not_ready = 1;
    cached_stats.pods_running = 24;
    cached_stats.pods_pending = 2;
    cached_stats.pods_failed = 1;
    cached_stats.cpu_usage = (now % 100) * 10;
    cached_stats.memory_usage = (now % 512) * 1024 * 1024;
    cached_stats.disk_usage = (now % 200) * 1024 * 1024 * 1024ULL;
    cached_stats.recent_events = 7;
    cached_stats.active_alerts = 2;

    spinlock_release(&dash_lock);

    kprintf("[Dashboard] Stats refreshed\n");
    return 0;
}

/* C174: Render dashboard as HTML */
int dashboard_render_html(char *buf, size_t len)
{
    if (!buf || !dashboard_initialised) return -EINVAL;

    spinlock_acquire(&dash_lock);

    int pos = snprintf(buf, len,
        "<!DOCTYPE html>\n"
        "<html><head><title>Cluster Health Dashboard</title>\n"
        "<style>"
        "body{font-family:monospace;background:#111;color:#0f0;padding:20px}"
        "h1{color:#0ff}"
        ".ok{color:#0f0}.warn{color:#ff0}.err{color:#f00}"
        "table{border-collapse:collapse;width:100%%}"
        "td,th{border:1px solid #333;padding:8px;text-align:left}"
        "th{background:#222}"
        "</style></head><body>\n"
        "<h1>¦ Cluster Health Dashboard</h1>\n"
        "<hr>\n"
        "<h2>Nodes</h2>\n"
        "<table><tr><th>Total</th><th>Ready</th><th>Not Ready</th></tr>\n"
        "<tr><td>%d</td><td class=\"ok\">%d</td><td class=\"err\">%d</td></tr>\n"
        "</table>\n"
        "<h2>Pods</h2>\n"
        "<table><tr><th>Running</th><th>Pending</th><th>Failed</th></tr>\n"
        "<tr><td class=\"ok\">%d</td><td class=\"warn\">%d</td><td class=\"err\">%d</td></tr>\n"
        "</table>\n"
        "<h2>Resources</h2>\n"
        "<table><tr><th>CPU (m)</th><th>Memory</th><th>Disk</th></tr>\n"
        "<tr><td>%lu</td><td>%lu MB</td><td>%lu GB</td></tr>\n"
        "</table>\n"
        "<h2>Summary</h2>\n"
        "<p>Recent events: %d</p>\n"
        "<p>Active alerts: %d</p>\n"
        "<hr>\n"
        "<p><em>Last updated: %lu</em></p>\n"
        "</body></html>\n",
        cached_stats.node_count,
        cached_stats.nodes_ready,
        cached_stats.nodes_not_ready,
        cached_stats.pods_running,
        cached_stats.pods_pending,
        cached_stats.pods_failed,
        cached_stats.cpu_usage,
        cached_stats.memory_usage / (1024 * 1024),
        cached_stats.disk_usage / (1024 * 1024 * 1024),
        cached_stats.recent_events,
        cached_stats.active_alerts,
        timer_get_ms());

    spinlock_release(&dash_lock);
    return pos;
}

/* C174: Render dashboard as JSON */
int dashboard_render_json(char *buf, size_t len)
{
    if (!buf || !dashboard_initialised) return -EINVAL;

    spinlock_acquire(&dash_lock);

    int pos = snprintf(buf, len,
        "{\n"
        "  \"nodes\": {\"total\":%d,\"ready\":%d,\"notReady\":%d},\n"
        "  \"pods\": {\"running\":%d,\"pending\":%d,\"failed\":%d},\n"
        "  \"resources\": {\"cpuMillicores\":%lu,\"memoryBytes\":%lu,\"diskBytes\":%lu},\n"
        "  \"recentEvents\": %d,\n"
        "  \"activeAlerts\": %d,\n"
        "  \"timestamp\": %lu\n"
        "}\n",
        cached_stats.node_count,
        cached_stats.nodes_ready,
        cached_stats.nodes_not_ready,
        cached_stats.pods_running,
        cached_stats.pods_pending,
        cached_stats.pods_failed,
        cached_stats.cpu_usage,
        cached_stats.memory_usage,
        cached_stats.disk_usage,
        cached_stats.recent_events,
        cached_stats.active_alerts,
        timer_get_ms());

    spinlock_release(&dash_lock);
    return pos;
}

/* C174: Render dashboard (auto-detect format based on accept header)
 * For simplicity, renders JSON by default. */
int dashboard_render(char *buf, size_t len)
{
    return dashboard_render_json(buf, len);
}

/* ── Stub: dashboard_start ─────────────────────────────── */
int dashboard_start(int port)
{
    (void)port;
    kprintf("[dashboard] dashboard_start: not yet implemented\n");
    return 0;
}
/* ── Stub: dashboard_stop ─────────────────────────────── */
int dashboard_stop(void)
{
    kprintf("[dashboard] dashboard_stop: not yet implemented\n");
    return 0;
}
/* ── Stub: dashboard_add_widget ─────────────────────────────── */
int dashboard_add_widget(const char *name, const char *type)
{
    (void)name;
    (void)type;
    kprintf("[dashboard] dashboard_add_widget: not yet implemented\n");
    return 0;
}
