/*
 * ima_policy.c — IMA Policy Engine
 *
 * Implements a policy evaluation engine for the Integrity Measurement
 * Architecture (IMA).  Policies determine which files are measured,
 * appraised, or audited based on criteria such as:
 *   - Path prefix matching (e.g., /bin/)
 *   - UID/GID matching
 *   - Filesystem magic number
 *   - Security label (SELinux type)
 *   - Executable vs mmap vs open
 *
 * Policy rules are stored in a kernel list and evaluated in order.
 * The first matching rule determines the action for the file.
 *
 * Item S100 — IMA policy engine
 */

#include "types.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"

/* ── IMA policy actions ──────────────────────────────────────────── */
#define IMA_POLICY_MEASURE   0x01   /* Measure the file */
#define IMA_POLICY_APPRAISE  0x02   /* Appraise the file */
#define IMA_POLICY_AUDIT     0x04   /* Audit access */
#define IMA_POLICY_DONT_MEASURE 0x08 /* Skip measurement */
#define IMA_POLICY_DONT_APPRAISE 0x10 /* Skip appraisal */

/* ── IMA policy flags (match criteria) ───────────────────────────── */
#define IMA_POLICY_FLAG_PATH       0x01   /* Match by path prefix */
#define IMA_POLICY_FLAG_UID        0x02   /* Match by UID */
#define IMA_POLICY_FLAG_GID        0x04   /* Match by GID */
#define IMA_POLICY_FLAG_MAGIC      0x08   /* Match by fs magic */
#define IMA_POLICY_FLAG_EXEC       0x10   /* Match exec file */
#define IMA_POLICY_FLAG_MMAP       0x20   /* Match mmap'd file */
#define IMA_POLICY_FLAG_MASK       0x40   /* Match by security label */

/* ── Policy rule ─────────────────────────────────────────────────── */
#define IMA_MAX_RULES      64
#define IMA_PATH_MAX       128

struct ima_policy_rule {
    uint32_t flags;               /* Which criteria to check (IMA_POLICY_FLAG_*) */
    uint32_t action;              /* Action(s) to take */
    char     path_prefix[IMA_PATH_MAX];  /* Path prefix match */
    uint32_t uid;                 /* UID to match */
    uint32_t gid;                 /* GID to match */
    uint32_t fs_magic;            /* Filesystem magic */
    uint32_t func;                /* Function type: exec, mmap, open */
};

/* ── Global policy table ─────────────────────────────────────────── */
static struct ima_policy_rule g_ima_rules[IMA_MAX_RULES];
static int g_ima_rule_count = 0;
static int g_ima_policy_initialized = 0;

/* ── Default built-in policies ───────────────────────────────────── */

/*
 * Default policy:
 *   Rule 1: Measure everything under /bin/ and /sbin/
 *   Rule 2: Measure everything under /etc/
 *   Rule 3: Measure everything under /lib/
 *   Rule 4: Appraise kernel modules under /lib/modules/
 *   Rule 5: Measure all exec'd files
 *   Rule 6: Fallback: measure everything
 */
static int ima_policy_add_rule(const char *rule);
static void ima_default_policy(void)
{
    /* 1. Measure binaries */
    ima_policy_add_rule("measure path=/bin/");
    ima_policy_add_rule("measure path=/sbin/");
    ima_policy_add_rule("measure path=/usr/bin/");
    ima_policy_add_rule("measure path=/usr/sbin/");

    /* 2. Measure config files */
    ima_policy_add_rule("measure path=/etc/");

    /* 3. Measure libraries */
    ima_policy_add_rule("measure path=/lib/");
    ima_policy_add_rule("measure path=/usr/lib/");

    /* 4. Appraise kernel modules */
    ima_policy_add_rule("measure path=/lib/modules/");
    ima_policy_add_rule("appraise path=/lib/modules/");

    /* 5. All executables get measured */
    ima_policy_add_rule("measure func=FILE_CHECK");

    /* 6. Appraise critical system files */
    ima_policy_add_rule("appraise path=/bin/");
    ima_policy_add_rule("appraise path=/sbin/");
}

/* ── API ─────────────────────────────────────────────────────────── */

/*
 * ima_policy_add_rule — Add a policy rule from a string.
 *
 * Format: "measure path=/bin/" or "appraise path=/sbin/"
 *         "measure uid=0" or "appraise func=FILE_CHECK"
 *
 * Returns 0 on success, -1 on failure.
 */
int ima_policy_add_rule(const char *rule_str)
{
    if (!rule_str || g_ima_rule_count >= IMA_MAX_RULES)
        return -1;

    struct ima_policy_rule *rule = &g_ima_rules[g_ima_rule_count];
    memset(rule, 0, sizeof(*rule));

    const char *p = rule_str;

    /* Parse action */
    if (strncmp(p, "measure", 7) == 0) {
        rule->action |= IMA_POLICY_MEASURE;
        p += 7;
    } else if (strncmp(p, "appraise", 8) == 0) {
        rule->action |= IMA_POLICY_APPRAISE;
        p += 8;
    } else if (strncmp(p, "dont_measure", 12) == 0) {
        rule->action |= IMA_POLICY_DONT_MEASURE;
        p += 12;
    } else if (strncmp(p, "dont_appraise", 13) == 0) {
        rule->action |= IMA_POLICY_DONT_APPRAISE;
        p += 13;
    } else {
        return -1;
    }

    /* Skip spaces */
    while (*p == ' ') p++;

    /* Parse key=value pairs */
    while (*p && *p != '\n') {
        /* Parse key */
        const char *key_start = p;
        while (*p && *p != '=') p++;
        if (*p != '=') break;
        size_t key_len = (size_t)(p - key_start);
        p++; /* skip '=' */

        /* Parse value */
        const char *val_start = p;
        while (*p && *p != ' ' && *p != '\n') p++;

        size_t val_len = (size_t)(p - val_start);

        /* Compare key */
        if (key_len == 4 && strncmp(key_start, "path", 4) == 0) {
            size_t copy = val_len < IMA_PATH_MAX - 1 ? val_len : IMA_PATH_MAX - 1;
            memcpy(rule->path_prefix, val_start, copy);
            rule->path_prefix[copy] = '\0';
            rule->flags |= IMA_POLICY_FLAG_PATH;
        } else if (key_len == 3 && strncmp(key_start, "uid", 3) == 0) {
            /* Simple number parse */
            uint32_t val = 0;
            for (size_t i = 0; i < val_len && val_start[i] >= '0' && val_start[i] <= '9'; i++)
                val = val * 10 + (uint32_t)(val_start[i] - '0');
            rule->uid = val;
            rule->flags |= IMA_POLICY_FLAG_UID;
        } else if (key_len == 3 && strncmp(key_start, "gid", 3) == 0) {
            uint32_t val = 0;
            for (size_t i = 0; i < val_len && val_start[i] >= '0' && val_start[i] <= '9'; i++)
                val = val * 10 + (uint32_t)(val_start[i] - '0');
            rule->gid = val;
            rule->flags |= IMA_POLICY_FLAG_GID;
        } else if (key_len == 4 && strncmp(key_start, "func", 4) == 0) {
            if (val_len == 9 && strncmp(val_start, "FILE_CHECK", 9) == 0)
                rule->func = 1;
            else if (val_len == 3 && strncmp(val_start, "MMAP", 3) == 0)
                rule->func = 2;
            else if (val_len == 4 && strncmp(val_start, "FILE_MMAP", 4) == 0)
                rule->func = 2;
            rule->flags |= IMA_POLICY_FLAG_EXEC;
        }

        /* Skip whitespace */
        while (*p == ' ') p++;
    }

    g_ima_rule_count++;
    return 0;
}

/*
 * ima_policy_evaluate — Evaluate policy for a given file path.
 *
 * Checks all rules in order.  Returns the action mask for the
 * first matching rule, or 0 if no rule matches (which means
 * "no action").
 *
 * @path:   Full path to the file being accessed.
 * @uid:    UID of the process accessing the file.
 * @gid:    GID of the process.
 * @func:   Function type (1=FILE_CHECK, 2=MMAP).
 * @fs_magic: Filesystem magic number (0 = unknown).
 *
 * Returns action bitmask (IMA_POLICY_MEASURE, etc.).
 */
uint32_t ima_policy_evaluate(const char *path, uint32_t uid, uint32_t gid,
                              uint32_t func, uint32_t fs_magic)
{
    if (!g_ima_policy_initialized || !path)
        return 0;

    for (int i = 0; i < g_ima_rule_count; i++) {
        const struct ima_policy_rule *rule = &g_ima_rules[i];
        int match = 1;

        /* Check path prefix */
        if (rule->flags & IMA_POLICY_FLAG_PATH) {
            if (strncmp(path, rule->path_prefix, strlen(rule->path_prefix)) != 0)
                match = 0;
        }

        /* Check UID */
        if (match && (rule->flags & IMA_POLICY_FLAG_UID)) {
            if (uid != rule->uid)
                match = 0;
        }

        /* Check GID */
        if (match && (rule->flags & IMA_POLICY_FLAG_GID)) {
            if (gid != rule->gid)
                match = 0;
        }

        /* Check function type */
        if (match && (rule->flags & IMA_POLICY_FLAG_EXEC)) {
            if (func != rule->func)
                match = 0;
        }

        /* Check filesystem magic */
        if (match && (rule->flags & IMA_POLICY_FLAG_MAGIC)) {
            if (fs_magic != rule->fs_magic)
                match = 0;
        }

        if (match)
            return rule->action;
    }

    /* Default: no action */
    return 0;
}

/*
 * ima_policy_should_measure — Convenience wrapper.
 * Returns 1 if the file should be measured, 0 otherwise.
 */
int ima_policy_should_measure(const char *path, uint32_t uid, uint32_t gid,
                               uint32_t func, uint32_t fs_magic)
{
    uint32_t action = ima_policy_evaluate(path, uid, gid, func, fs_magic);
    if (action & IMA_POLICY_DONT_MEASURE)
        return 0;
    return (action & IMA_POLICY_MEASURE) ? 1 : 0;
}

/*
 * ima_policy_should_appraise — Convenience wrapper.
 * Returns 1 if the file should be appraised, 0 otherwise.
 */
int ima_policy_should_appraise(const char *path, uint32_t uid, uint32_t gid,
                                uint32_t func, uint32_t fs_magic)
{
    uint32_t action = ima_policy_evaluate(path, uid, gid, func, fs_magic);
    if (action & IMA_POLICY_DONT_APPRAISE)
        return 0;
    return (action & IMA_POLICY_APPRAISE) ? 1 : 0;
}

/*
 * ima_policy_init — Initialize the IMA policy engine.
 * Loads the default built-in policy rules.
 */
void ima_policy_init(void)
{
    if (g_ima_policy_initialized) return;

    memset(g_ima_rules, 0, sizeof(g_ima_rules));
    g_ima_rule_count = 0;

    /* Load default policy */
    ima_default_policy();

    g_ima_policy_initialized = 1;
    kprintf("[OK] IMA policy engine initialized (%d rules)\n", g_ima_rule_count);
}

/*
 * ima_policy_get_rule_count — Return the number of active policy rules.
 */
int ima_policy_get_rule_count(void)
{
    return g_ima_rule_count;
}

/*
 * ima_policy_clear — Clear all policy rules.
 */
void ima_policy_clear(void)
{
    memset(g_ima_rules, 0, sizeof(g_ima_rules));
    g_ima_rule_count = 0;
}
#include "module.h"
module_init(ima_policy_init);
