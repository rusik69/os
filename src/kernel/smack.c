/*
 * smack.c — Simplified Mandatory Access Control Kernel
 *
 * Implements label-based mandatory access control:
 *   - Every subject (process) has a SMACK label
 *   - Every object (inode/file) has a SMACK label (via security.smack64 xattr)
 *   - Access is granted if there is a rule "subject_label object_label rwx"
 *     that matches both labels and covers the requested access
 *
 * Default labels:
 *   "_" (floor) — can access anything, but other labels cannot access it
 *   "*" (star)  — can be accessed by any subject
 *   "^" (hat)   — only the same label can access it
 *
 * Static rule table (max 64 rules) with /sys/fs/smackfs/load interface.
 */

#define KERNEL_INTERNAL
#include "smack.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "sysfs.h"
#include "errno.h"
#include "xattr.h"
#include "process.h"
#include "spinlock.h"

/* ── State ──────────────────────────────────────────────────────────── */

/* Default label for unlabeled entities */
static char default_label[SMACK_LABEL_LEN] = SMACK_LABEL_FLOOR;

/* Per-process label storage (simple: track up to 32 process labels) */
struct smack_subject {
    uint32_t pid;
    char label[SMACK_LABEL_LEN];
};

static struct smack_subject smack_subjects[SMACK_MAX_SUBJECTS];
static int smack_subject_count = 0;

/* Rule table */
static struct smack_rule smack_rules[SMACK_MAX_RULES];
static int smack_rule_count = 0;

/* Lock for concurrency */
static spinlock_t smack_lock = 0;

/* Are we initialized? */
static int smack_initialized = 0;

/* ── Internal helpers ──────────────────────────────────────────────── */

/* Find a subject entry by PID, or return NULL */
static struct smack_subject *smack_find_subject(uint32_t pid)
{
    for (int i = 0; i < smack_subject_count; i++) {
        if (smack_subjects[i].pid == pid)
            return &smack_subjects[i];
    }
    return NULL;
}

/* Get or create a subject entry for the given PID */
static struct smack_subject *smack_get_or_create_subject(uint32_t pid)
{
    struct smack_subject *sub = smack_find_subject(pid);
    if (sub)
        return sub;

    if (smack_subject_count >= SMACK_MAX_SUBJECTS)
        return NULL;

    sub = &smack_subjects[smack_subject_count++];
    sub->pid = pid;
    strncpy(sub->label, default_label, SMACK_LABEL_LEN - 1);
    sub->label[SMACK_LABEL_LEN - 1] = '\0';
    return sub;
}

/* Parse access string like "rwx" into bitmask */
static uint8_t smack_parse_access(const char *str)
{
    uint8_t mask = 0;
    while (str && *str) {
        switch (*str) {
            case 'r': mask |= SMACK_MAY_READ; break;
            case 'w': mask |= SMACK_MAY_WRITE; break;
            case 'x': mask |= SMACK_MAY_EXEC; break;
            case 'a': mask |= SMACK_MAY_APPEND; break;
            case 't': mask |= SMACK_MAY_TRANSMUTE; break;
        }
        str++;
    }
    return mask;
}

/* Check if a character is valid in a label */
static int smack_valid_label_char(char c)
{
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    if (c == '_' || c == '-' || c == '.' || c == '*' || c == '^') return 1;
    return 0;
}

/* Validate a label string */
static int smack_validate_label(const char *label)
{
    if (!label || !*label)
        return -EINVAL;

    int len = (int)strlen(label);
    if (len >= SMACK_LABEL_LEN)
        return -ENAMETOOLONG;

    for (int i = 0; i < len; i++) {
        if (!smack_valid_label_char(label[i]))
            return -EINVAL;
    }
    return 0;
}

/* ── Initialization ────────────────────────────────────────────────── */

void smack_init(void)
{
    if (smack_initialized)
        return;

    memset(smack_subjects, 0, sizeof(smack_subjects));
    memset(smack_rules, 0, sizeof(smack_rules));
    smack_subject_count = 0;
    smack_rule_count = 0;

    /* Set default label */
    strncpy(default_label, SMACK_LABEL_FLOOR, SMACK_LABEL_LEN - 1);
    default_label[SMACK_LABEL_LEN - 1] = '\0';

    /* Add default rules:
     *   Floor can access everything
     *   Star can be accessed by everything
     *   Hat restricts to same label
     */
    smack_add_rule(SMACK_LABEL_FLOOR, SMACK_LABEL_STAR, "rwx");
    smack_add_rule(SMACK_LABEL_STAR, SMACK_LABEL_STAR, "rwx");
    smack_add_rule(SMACK_LABEL_STAR, SMACK_LABEL_FLOOR, "rwx");

    smack_sysfs_init();

    smack_initialized = 1;
    kprintf("[OK] SMACK initialized (floor=_, star=*, hat=^)\n");
}

/* ── Label management ──────────────────────────────────────────────── */

int smack_set_process_label(const char *label)
{
    struct process *cur = process_get_current();
    if (!cur)
        return -ESRCH;

    if (label) {
        int ret = smack_validate_label(label);
        if (ret < 0)
            return ret;
    }

    spinlock_acquire(&smack_lock);

    struct smack_subject *sub = smack_get_or_create_subject(cur->pid);
    if (!sub) {
        spinlock_release(&smack_lock);
        return -ENOMEM;
    }

    if (label) {
        strncpy(sub->label, label, SMACK_LABEL_LEN - 1);
        sub->label[SMACK_LABEL_LEN - 1] = '\0';
    } else {
        strncpy(sub->label, default_label, SMACK_LABEL_LEN - 1);
        sub->label[SMACK_LABEL_LEN - 1] = '\0';
    }

    spinlock_release(&smack_lock);
    return 0;
}

const char *smack_get_process_label(void)
{
    struct process *cur = process_get_current();
    if (!cur)
        return default_label;

    spinlock_acquire(&smack_lock);
    struct smack_subject *sub = smack_find_subject(cur->pid);
    spinlock_release(&smack_lock);

    if (sub)
        return sub->label;
    return default_label;
}

int smack_set_file_label(const char *path, const char *label)
{
    if (!path || !label)
        return -EINVAL;

    int ret = smack_validate_label(label);
    if (ret < 0)
        return ret;

    return xattr_set(path, "security.smack64", label, strlen(label) + 1);
}

int smack_get_file_label(const char *path, char *label, int label_len)
{
    if (!path || !label || label_len <= 0)
        return -EINVAL;

    size_t size = (size_t)label_len;
    int ret = xattr_get(path, "security.smack64", label, size);
    if (ret < 0)
        return ret;

    /* Ensure null-termination */
    if ((size_t)ret < (size_t)label_len)
        label[ret] = '\0';
    else
        label[label_len - 1] = '\0';

    return 0;
}

/* ── Rule management ───────────────────────────────────────────────── */

int smack_add_rule(const char *subject, const char *object, const char *access_str)
{
    if (!subject || !object || !access_str)
        return -EINVAL;

    int ret = smack_validate_label(subject);
    if (ret < 0) return ret;
    ret = smack_validate_label(object);
    if (ret < 0) return ret;

    uint8_t access = smack_parse_access(access_str);
    if (access == 0)
        return -EINVAL;

    spinlock_acquire(&smack_lock);

    if (smack_rule_count >= SMACK_MAX_RULES) {
        spinlock_release(&smack_lock);
        return -ENOSPC;
    }

    struct smack_rule *rule = &smack_rules[smack_rule_count++];
    strncpy(rule->subject, subject, SMACK_LABEL_LEN - 1);
    rule->subject[SMACK_LABEL_LEN - 1] = '\0';
    strncpy(rule->object, object, SMACK_LABEL_LEN - 1);
    rule->object[SMACK_LABEL_LEN - 1] = '\0';
    rule->access = access;

    spinlock_release(&smack_lock);

    kprintf("[SMACK] rule added: '%s' '%s' %02x\n", subject, object, access);
    return 0;
}

void smack_clear_rules(void)
{
    spinlock_acquire(&smack_lock);
    memset(smack_rules, 0, sizeof(smack_rules));
    smack_rule_count = 0;
    spinlock_release(&smack_lock);
}

int smack_check_access(const char *subject, const char *object, uint8_t access)
{
    if (!subject || !object)
        return 0;  /* deny */

    /* Special label semantics:
     *   "*" (star) as object — any subject can access
     *   "_" (floor) as subject — can access any object (unless hat)
     *   "^" (hat) — only same subject can access
     *   subject == object — always allowed
     */

    if (strcmp(subject, object) == 0)
        return 1;  /* same label: always allowed */

    /* Hat check: if object is "^", only same label works (checked above) */
    if (strcmp(object, SMACK_LABEL_HAT) == 0)
        return 0;  /* different labels, hat denies */

    /* Star object: anyone can access */
    if (strcmp(object, SMACK_LABEL_STAR) == 0)
        return 1;

    /* Floor subject: can access anything that's not hat */
    if (strcmp(subject, SMACK_LABEL_FLOOR) == 0)
        return 1;

    /* Search rule table */
    spinlock_acquire(&smack_lock);
    for (int i = 0; i < smack_rule_count; i++) {
        if (strcmp(smack_rules[i].subject, subject) == 0 &&
            strcmp(smack_rules[i].object, object) == 0 &&
            (smack_rules[i].access & access) == access) {
            spinlock_release(&smack_lock);
            return 1;
        }
    }
    spinlock_release(&smack_lock);

    return 0;
}

/* ── LSM hook implementations ──────────────────────────────────────── */

int smack_cred_alloc_blank(void)
{
    struct process *cur = process_get_current();
    if (!cur)
        return 0;

    spinlock_acquire(&smack_lock);
    struct smack_subject *sub = smack_get_or_create_subject(cur->pid);
    if (sub) {
        /* Inherit parent label — this is a placeholder for new creds */
        strncpy(sub->label, default_label, SMACK_LABEL_LEN - 1);
        sub->label[SMACK_LABEL_LEN - 1] = '\0';
    }
    spinlock_release(&smack_lock);
    return 0;
}

int smack_bprm_set_creds(const char *filename)
{
    (void)filename;

    struct process *cur = process_get_current();
    if (!cur)
        return 0;

    /* On exec: inherit current label (no change by default).
     * A full implementation would check the file's SMACK64 xattr
     * and possibly transition the process label. */
    spinlock_acquire(&smack_lock);
    struct smack_subject *sub = smack_get_or_create_subject(cur->pid);
    if (!sub) {
        spinlock_release(&smack_lock);
        return -ENOMEM;
    }

    /* Check if the executable has a SMACK64 xattr for label transition */
    char exec_label[SMACK_LABEL_LEN];
    int ret = smack_get_file_label(cur->exe_path, exec_label, sizeof(exec_label));
    if (ret == 0 && exec_label[0] != '\0') {
        /* Transition to the executable's label if it's different */
        if (strcmp(sub->label, exec_label) != 0) {
            kprintf("[SMACK] exec label transition '%s' -> '%s'\n",
                    sub->label, exec_label);
            strncpy(sub->label, exec_label, SMACK_LABEL_LEN - 1);
            sub->label[SMACK_LABEL_LEN - 1] = '\0';
        }
    }

    spinlock_release(&smack_lock);
    return 0;
}

int smack_inode_permission(const char *path, int mask)
{
    if (!path)
        return 0;

    /* Convert VFS mask (R_OK=4, W_OK=2, X_OK=1) to SMACK flags */
    uint8_t smack_access = 0;
    if (mask & 4) smack_access |= SMACK_MAY_READ;
    if (mask & 2) smack_access |= SMACK_MAY_WRITE;
    if (mask & 1) smack_access |= SMACK_MAY_EXEC;

    if (smack_access == 0)
        return 0;  /* no access requested, allow */

    /* Get the process label */
    const char *proc_label = smack_get_process_label();

    /* Get the file's SMACK label */
    char file_label[SMACK_LABEL_LEN];
    int ret = smack_get_file_label(path, file_label, sizeof(file_label));
    if (ret < 0) {
        /* No label on file: use default label */
        strncpy(file_label, default_label, SMACK_LABEL_LEN - 1);
        file_label[SMACK_LABEL_LEN - 1] = '\0';
    }

    /* Check access */
    if (!smack_check_access(proc_label, file_label, smack_access)) {
        kprintf("[SMACK] inode_permission DENIED: '%s' -> '%s' (path=%s, mask=%d)\n",
                proc_label, file_label, path, mask);
        return -EACCES;
    }

    return 0;
}

int smack_file_permission(const char *path, int mask)
{
    /* For file operations, delegate to inode permission check */
    return smack_inode_permission(path, mask);
}

int smack_task_kill(uint32_t target_pid, int sig)
{
    (void)sig;

    const char *proc_label = smack_get_process_label();

    spinlock_acquire(&smack_lock);
    struct smack_subject *target = smack_find_subject(target_pid);
    spinlock_release(&smack_lock);

    if (!target) {
        /* Target has no label — allow (it's likely the init process or kernel) */
        return 0;
    }

    /* To kill: subject must have execute permission on the target's label.
     * This follows the standard SMACK "send signal" semantics. */
    if (!smack_check_access(proc_label, target->label, SMACK_MAY_EXEC)) {
        kprintf("[SMACK] task_kill DENIED: '%s' cannot kill pid %u (label '%s')\n",
                proc_label, target_pid, target->label);
        return -EACCES;
    }

    return 0;
}

/* ── Sysfs interface ───────────────────────────────────────────────── */

/* Read callback for /sys/fs/smackfs/load — lists current rules */
static int smack_load_read(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    int pos = 0;
    int n;

    spinlock_acquire(&smack_lock);

    n = snprintf(buf + pos, (size_t)((int)max_size - pos),
                 "# SMACK rules (%d/%d)\n", smack_rule_count, SMACK_MAX_RULES);
    if (n > 0 && pos + n < (int)max_size) pos += n;

    for (int i = 0; i < smack_rule_count && pos < (int)max_size - 80; i++) {
        char access_str[6];
        int ap = 0;
        if (smack_rules[i].access & SMACK_MAY_READ)  access_str[ap++] = 'r';
        if (smack_rules[i].access & SMACK_MAY_WRITE) access_str[ap++] = 'w';
        if (smack_rules[i].access & SMACK_MAY_EXEC)  access_str[ap++] = 'x';
        if (smack_rules[i].access & SMACK_MAY_APPEND) access_str[ap++] = 'a';
        if (smack_rules[i].access & SMACK_MAY_TRANSMUTE) access_str[ap++] = 't';
        access_str[ap] = '\0';

        n = snprintf(buf + pos, (size_t)((int)max_size - pos),
                     "%s %s %s\n",
                     smack_rules[i].subject,
                     smack_rules[i].object,
                     access_str);
        if (n > 0 && pos + n < (int)max_size) pos += n;
    }

    if (pos < (int)max_size)
        buf[pos] = '\0';

    spinlock_release(&smack_lock);
    return pos;
}

/* Write callback for /sys/fs/smackfs/load — add rules */
static int smack_load_write(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    if (!data || size == 0)
        return 0;

    /* Parse "subject_label object_label access_string" lines */
    char line[128];
    int pos = 0;

    while (pos < (int)size) {
        int line_len = 0;
        while (pos < (int)size && data[pos] != '\n' && line_len < (int)sizeof(line) - 1) {
            line[line_len++] = data[pos++];
        }
        if (pos < (int)size && data[pos] == '\n')
            pos++;  /* skip newline */
        line[line_len] = '\0';

        /* Skip empty lines and comments */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Parse: tokenize by spaces */
        char *saveptr = NULL;
        char *subject = strtok_r(line, " \t", &saveptr);
        if (!subject) continue;
        char *object = strtok_r(NULL, " \t", &saveptr);
        if (!object) continue;
        char *access_str = strtok_r(NULL, " \t", &saveptr);
        if (!access_str) continue;

        smack_add_rule(subject, object, access_str);
    }

    return 0;
}

void smack_sysfs_init(void)
{
    /* Create /sys/fs directory */
    sysfs_create_dir("/sys/fs");

    /* Create /sys/fs/smackfs directory */
    sysfs_create_dir("/sys/fs/smackfs");

    /* Create load file with read/write callbacks */
    sysfs_create_writable_file("/sys/fs/smackfs/load",
                               "# SMACK rules\n", NULL,
                               smack_load_read, smack_load_write);

    kprintf("[SMACK] /sys/fs/smackfs/load created\n");
}
/* ── Stub: smack_task_ptrace ─────────────────────────────── */
int smack_task_ptrace(struct process *tracer, struct process *tracee)
{
    (void)tracer;
    (void)tracee;
    kprintf("[smack] smack_task_ptrace: not yet implemented\n");
    return 0;
}

/* ── Stub: smack_setprocattr ─────────────────────────────── */
int smack_setprocattr(const char *attr, const char *value, size_t size)
{
    (void)attr;
    (void)value;
    (void)size;
    kprintf("[smack] smack_setprocattr: not yet implemented\n");
    return 0;
}

/* ── Stub: smack_getprocattr ─────────────────────────────── */
int smack_getprocattr(const char *attr, char *buf, size_t size)
{
    (void)attr;
    (void)buf;
    (void)size;
    kprintf("[smack] smack_getprocattr: not yet implemented\n");
    return 0;
}

/* ── Stub: smack_netlabel ─────────────────────────────── */
int smack_netlabel(const char *label, const char *addr, int family)
{
    (void)label;
    (void)addr;
    (void)family;
    kprintf("[smack] smack_netlabel: not yet implemented\n");
    return 0;
}

/* ── Stub: smack_cipso ─────────────────────────────── */
int smack_cipso(const char *label, const void *doi, const void *cipso)
{
    (void)label;
    (void)doi;
    (void)cipso;
    kprintf("[smack] smack_cipso: not yet implemented\n");
    return 0;
}

/* ── Stub: smack_from_secattr ─────────────────────────────── */
int smack_from_secattr(const void *secattr, char *label, size_t label_len)
{
    (void)secattr;
    (void)label;
    (void)label_len;
    kprintf("[smack] smack_from_secattr: not yet implemented\n");
    return 0;
}

#include "module.h"
module_init(smack_init);
