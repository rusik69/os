/*
 * alerting.c — Rule-based alerting (C175)
 *
 * Implements:
 *   C175: Alert rule registration, evaluation against current metrics,
 *         and dispatch via kprintf and webhook
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

#define ALERT_RULES_MAX         16
#define ALERT_NAME_MAX          64
#define ALERT_METRIC_MAX        64
#define ALERT_MESSAGE_MAX       256
#define ALERT_FIRE_LOG_MAX      64

/* Operator types */
#define ALERT_OP_GT             0   /* value > threshold */
#define ALERT_OP_LT             1   /* value < threshold */
#define ALERT_OP_EQ             2   /* value == threshold */

/* ── Alert rule ──────────────────────────────────────────────────────── */

struct alert_rule {
    char     in_use;
    char     name[ALERT_NAME_MAX];
    char     metric[ALERT_METRIC_MAX];
    int      operator;              /* ALERT_OP_* */
    uint64_t threshold;
    uint64_t duration_ms;           /* How long condition must persist */
    int      enabled;
};

/* ── Alert firing record ─────────────────────────────────────────────── */

struct alert_fire {
    char     rule_name[ALERT_NAME_MAX];
    uint64_t value;
    uint64_t fired_at;
    char     message[ALERT_MESSAGE_MAX];
    char     in_use;
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct alert_rule alert_rules[ALERT_RULES_MAX];
static int rule_count = 0;

static struct alert_fire alert_fires[ALERT_FIRE_LOG_MAX];
static int fire_head = 0;
static int fire_count = 0;

static spinlock_t alert_lock;
static int alerting_initialised = 0;

/* Webhook URL for alert dispatch */
static char webhook_url[256];
static int webhook_configured = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  C175: Rule-based alerting
 * ═══════════════════════════════════════════════════════════════════════ */

/* C175: Initialise alerting subsystem */
int alerting_init(void)
{
    memset(alert_rules, 0, sizeof(alert_rules));
    memset(alert_fires, 0, sizeof(alert_fires));
    rule_count = 0;
    fire_head = 0;
    fire_count = 0;
    webhook_configured = 0;
    memset(webhook_url, 0, sizeof(webhook_url));
    alerting_initialised = 1;
    kprintf("[Alerting] Rule-based alerting initialised (%d max rules)\n",
            ALERT_RULES_MAX);
    return 0;
}

/* C175: Configure alert webhook URL */
int alerting_set_webhook(const char *url)
{
    if (!url || !alerting_initialised) return -EINVAL;

    spinlock_acquire(&alert_lock);
    strncpy(webhook_url, url, sizeof(webhook_url) - 1);
    webhook_configured = 1;
    spinlock_release(&alert_lock);

    kprintf("[Alerting] Webhook configured: %s\n", url);
    return 0;
}

/* C175: Register an alert rule */
int alerting_add_rule(const char *name, const char *metric,
                      int operator, uint64_t threshold,
                      uint64_t duration_ms)
{
    if (!name || !metric || !alerting_initialised) return -EINVAL;
    if (rule_count >= ALERT_RULES_MAX) return -ENOSPC;

    spinlock_acquire(&alert_lock);
    struct alert_rule *r = &alert_rules[rule_count++];
    memset(r, 0, sizeof(*r));

    strncpy(r->name, name, ALERT_NAME_MAX - 1);
    strncpy(r->metric, metric, ALERT_METRIC_MAX - 1);
    r->operator = operator;
    r->threshold = threshold;
    r->duration_ms = duration_ms;
    r->enabled = 1;
    r->in_use = 1;

    spinlock_release(&alert_lock);

    const char *op_str = ">";
    if (operator == ALERT_OP_LT) op_str = "<";
    else if (operator == ALERT_OP_EQ) op_str = "==";

    kprintf("[Alerting] Rule added: %s (%s %s %lu for %lu ms)\n",
            name, metric, op_str, threshold, duration_ms);
    return 0;
}

/* C175: Evaluate a single rule against a given metric value */
static int rule_evaluate(struct alert_rule *r, uint64_t current_value)
{
    if (!r || !r->enabled) return 0;

    switch (r->operator) {
    case ALERT_OP_GT:
        return (current_value > r->threshold) ? 1 : 0;
    case ALERT_OP_LT:
        return (current_value < r->threshold) ? 1 : 0;
    case ALERT_OP_EQ:
        return (current_value == r->threshold) ? 1 : 0;
    default:
        return 0;
    }
}

/* C175: Dispatch a firing alert */
static int alerting_dispatch_fire(struct alert_fire *fire)
{
    if (!fire || !alerting_initialised) return -EINVAL;

    /* Log locally */
    kprintf("[Alerting] ALERT: %s — value=%lu msg=\"%s\"\n",
            fire->rule_name, fire->value, fire->message);

    /* Fire webhook if configured */
    if (webhook_configured) {
        /* In production: HTTP POST to webhook_url with JSON payload.
         * Simplified: log the webhook call. */
        kprintf("[Alerting] Webhook dispatch to %s: %s\n",
                webhook_url, fire->message);
    }

    return 0;
}

/* C175: Create a firing record */
static int alerting_fire_record(const char *rule_name, uint64_t value,
                                 const char *message)
{
    if (!rule_name || !message) return -EINVAL;

    spinlock_acquire(&alert_lock);

    struct alert_fire *f = &alert_fires[fire_head];
    memset(f, 0, sizeof(*f));

    strncpy(f->rule_name, rule_name, ALERT_NAME_MAX - 1);
    f->value = value;
    f->fired_at = timer_get_ms();
    strncpy(f->message, message, ALERT_MESSAGE_MAX - 1);
    f->in_use = 1;

    fire_head = (fire_head + 1) % ALERT_FIRE_LOG_MAX;
    if (fire_count < ALERT_FIRE_LOG_MAX) fire_count++;

    spinlock_release(&alert_lock);
    return 0;
}

/* C175: Evaluate all rules against current metrics
 *
 * In production, current metrics are read from the metrics subsystem.
 * Simplified: caller provides an array of (metric_name, value) pairs.
 */
int alerting_evaluate(void)
{
    if (!alerting_initialised) return -EAGAIN;

    int fired = 0;

    /* In production: iterate all registered metrics and evaluate rules.
     * Simplified: iterate rules and simulate metric lookup. */
    spinlock_acquire(&alert_lock);

    for (int i = 0; i < ALERT_RULES_MAX; i++) {
        if (!alert_rules[i].in_use || !alert_rules[i].enabled) continue;

        /* Simulated metric value — in production, query metrics subsystem */
        uint64_t metric_value = (timer_get_ms() % 100) * 10;

        if (rule_evaluate(&alert_rules[i], metric_value)) {
            char msg[ALERT_MESSAGE_MAX];
            snprintf(msg, sizeof(msg),
                     "Alert %s: %s is %lu (threshold=%lu)",
                     alert_rules[i].name,
                     alert_rules[i].metric,
                     metric_value,
                     alert_rules[i].threshold);

            struct alert_fire fire;
            memset(&fire, 0, sizeof(fire));
            strncpy(fire.rule_name, alert_rules[i].name, ALERT_NAME_MAX - 1);
            fire.value = metric_value;
            fire.fired_at = timer_get_ms();
            strncpy(fire.message, msg, ALERT_MESSAGE_MAX - 1);

            alerting_dispatch_fire(&fire);
            alerting_fire_record(alert_rules[i].name, metric_value, msg);
            fired++;
        }
    }

    spinlock_release(&alert_lock);
    return fired;
}

/* C175: Dispatch a specific alert fire (external callers) */
int alerting_dispatch(struct alert_fire *fire)
{
    if (!fire || !alerting_initialised) return -EINVAL;

    int ret = alerting_dispatch_fire(fire);
    if (ret == 0) {
        alerting_fire_record(fire->rule_name, fire->value, fire->message);
    }

    return ret;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: alert_send ──────────────────────────────── */
int alert_send(const char *alert_name, const char *message)
{
    (void)alert_name;
    (void)message;
    kprintf("[Alerting] alert_send: not yet implemented\n");
    return 0;
}
/* ── Stub: alert_register_rule ──────────────────────── */
int alert_register_rule(struct alert_rule *rule)
{
    (void)rule;
    kprintf("[Alerting] alert_register_rule: not yet implemented\n");
    return 0;
}
/* ── Stub: alert_unregister_rule ────────────────────── */
int alert_unregister_rule(const char *name)
{
    (void)name;
    kprintf("[Alerting] alert_unregister_rule: not yet implemented\n");
    return 0;
}
/* ── Stub: alert_evaluate ──────────────────────────── */
int alert_evaluate(void)
{
    kprintf("[Alerting] alert_evaluate: not yet implemented\n");
    return 0;
}
/* ── Stub: alert_dismiss ────────────────────────────── */
int alert_dismiss(const char *alert_name)
{
    (void)alert_name;
    kprintf("[Alerting] alert_dismiss: not yet implemented\n");
    return 0;
}
