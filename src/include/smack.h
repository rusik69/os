#ifndef SMACK_H
#define SMACK_H

/*
 * smack.h — Simplified Mandatory Access Control Kernel (SMACK)
 *
 * SMACK is a label-based mandatory access control system.
 * Each subject (process) and object (inode) carries a SMACK label.
 * Access is granted/denied based on a rule table that describes
 * the allowed interactions between labels.
 *
 * Labels are short ASCII strings (<= 24 chars + NUL).
 * Rules: "subject_label object_label rwx"
 *
 * Maximum: 64 rules, 32 subjects/objects tracked.
 */

#include "types.h"

/* ── Limits ──────────────────────────────────────────────────────── */
#define SMACK_LABEL_LEN      25   /* max label length including NUL */
#define SMACK_MAX_RULES      64   /* max rules in the table */
#define SMACK_MAX_SUBJECTS   32   /* max tracked subjects */
#define SMACK_MAX_OBJECTS    32   /* max tracked objects */

/* ── Access modes ────────────────────────────────────────────────── */
#define SMACK_MAY_READ    (1u << 0)
#define SMACK_MAY_WRITE   (1u << 1)
#define SMACK_MAY_EXEC    (1u << 2)
#define SMACK_MAY_APPEND  (1u << 3)
#define SMACK_MAY_TRANSMUTE (1u << 4)

/* ── Rule entry ──────────────────────────────────────────────────── */
struct smack_rule {
    char subject[SMACK_LABEL_LEN];   /* subject label */
    char object[SMACK_LABEL_LEN];    /* object label */
    uint8_t access;                   /* bitmask of SMACK_MAY_* */
};

/* ── Public API ──────────────────────────────────────────────────── */

/* Initialize the SMACK subsystem */
void smack_init(void);

/* ── Label management ───────────────────────────────────────────── */

/* Set a process's SMACK label (NULL = use default "_" label) */
int smack_set_process_label(const char *label);

/* Get the current process's SMACK label (returns pointer to static buffer) */
const char *smack_get_process_label(void);

/* Set an inode's SMACK label via security.smack64 xattr */
int smack_set_file_label(const char *path, const char *label);

/* Get an inode's SMACK label */
int smack_get_file_label(const char *path, char *label, int label_len);

/* ── Rule management ────────────────────────────────────────────── */

/* Add a rule: "subject_label object_label rwx" */
int smack_add_rule(const char *subject, const char *object, const char *access_str);

/* Clear all rules */
void smack_clear_rules(void);

/* Check if subject_label has 'access' permission on object_label */
int smack_check_access(const char *subject, const char *object, uint8_t access);

/* ── LSM hook implementations ───────────────────────────────────── */

/* Called when allocating blank creds for a new process */
int smack_cred_alloc_blank(void);

/* Called during exec to inherit/set process label */
int smack_bprm_set_creds(const char *filename);

/* Check inode access (read/write/execute) */
int smack_inode_permission(const char *path, int mask);

/* Check file access (open) */
int smack_file_permission(const char *path, int mask);

/* Check if a process can kill another process */
int smack_task_kill(uint32_t target_pid, int sig);

/* ── Sysfs interface ────────────────────────────────────────────── */

/* Create /sys/fs/smackfs/ entries (called from smack_init) */
void smack_sysfs_init(void);

/* Default SMACK label for unlabeled subjects/objects */
#define SMACK_LABEL_FLOOR   "_"
#define SMACK_LABEL_STAR    "*"
#define SMACK_LABEL_HAT     "^"

#endif /* SMACK_H */
