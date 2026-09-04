/*
 * audit_rule.c — Audit rule engine.
 *
 * Holds a small table of audit rules.  Each rule carries an optional set
 * of match criteria (syscall number, pid, path prefix); a rule is said to
 * match an event when every enabled criterion is satisfied.  The syscall
 * dispatcher consults these rules before emitting a SYSCALL record, so an
 * empty rule table (the default) means "audit everything" — matching
 * Linux's behaviour where the audit framework is engaged by rules.
 *
 * The structure is intentionally extensible: subsequent audit tasks add
 * more criterion types and the VFS path watch, reusing the same match
 * function.
 */

#include "audit.h"
#include "errno.h"
#include "string.h"

static struct audit_rule audit_rules[AUDIT_RULE_MAX];
static int audit_rules_used;

void audit_rule_clear(void) {
    memset(audit_rules, 0, sizeof(audit_rules));
    audit_rules_used = 0;
}

int audit_rule_add(const struct audit_rule *r) {
    if (!r)
        return -1;
    if (audit_rules_used >= AUDIT_RULE_MAX)
        return -1;

    audit_rules[audit_rules_used++] = *r;
    return audit_rules_used - 1;
}

int audit_rule_remove(int idx) {
    int i;

    if (idx < 0 || idx >= audit_rules_used)
        return -ENOENT;

    /* Shift remaining rules down. */
    for (i = idx; i < audit_rules_used - 1; i++)
        audit_rules[i] = audit_rules[i + 1];
    audit_rules_used--;

    return 0;
}

int audit_rule_matches(long syscall, uint32_t pid, const char *path) {
    int i;

    for (i = 0; i < audit_rules_used; i++) {
        const struct audit_rule *r = &audit_rules[i];

        /* Each enabled criterion must match; disabled criteria are
         * ignored (an "any" filter). */
        if ((r->match & AUDIT_MATCH_SYSCALL) && r->syscall >= 0 && r->syscall != syscall)
            continue;
        if ((r->match & AUDIT_MATCH_PID) && r->pid != 0 && r->pid != pid)
            continue;
        if ((r->match & AUDIT_MATCH_PATH) && r->path[0] != '\0') {
            if (!path || strncmp(path, r->path, strlen(r->path)) != 0)
                continue;
        }
        return 1; /* all enabled criteria matched */
    }
    return 0;
}

int audit_rules_filter_syscall(void) {
    int i;

    for (i = 0; i < audit_rules_used; i++) {
        if (audit_rules[i].match & AUDIT_MATCH_SYSCALL)
            return 1;
    }
    return 0;
}

int audit_rule_get(int idx, struct audit_rule *out) {
    if (idx < 0 || idx >= audit_rules_used || !out)
        return 0;
    *out = audit_rules[idx];
    return 1;
}

int audit_rule_count(void) {
    return audit_rules_used;
}