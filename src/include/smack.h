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

/* ── CIPSO (Common IP Security Option) label mapping ─────────────── */
/* Maps a CIPSO tag (a small numeric category id) to a Smack label so
 * labels can be carried on the wire.  A single default DOI is used, as
 * in classic Smack networking.  Also maps a label back to its tag. */
#define SMACK_CIPSO_MAX_TAGS 64

struct smack_cipso_map {
    int used;                    /* slot in use */
    uint8_t tag;                 /* CIPSO category number (1..) */
    int socket_matched;          /* informational */
    char label[SMACK_LABEL_LEN]; /* Smack label bound to the tag */
};

/* ── Tracked object labels ───────────────────────────────────────── */
/* A small cache of path → Smack-label bindings so the same object's
 * label is resolved consistently across repeated accesses. */
struct smack_object {
    char path[SMACK_LABEL_LEN];  /* object identifier (path) */
    char label[SMACK_LABEL_LEN]; /* resolved Smack label */
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

/* ── Subject / object label matching ─────────────────────────────── */

/* Resolve (and cache) the Smack label bound to an object path.  Falls
 * back to the default label when the object has no smack64 xattr.
 * Returns 0 and fills 'label' (<= label_len) on success. */
int smack_resolve_object_label(const char *path, char *label, int label_len);

/* Subject/object label matching: is the current process allowed the
 * requested 'access' mask on the given object path?  Resolves both the
 * subject (current process) and object labels, then consults the rule
 * table and the special star/floor/hat semantics.  Returns 1 (allow) or
 * 0 (deny). */
int smack_may_access(const char *path, uint8_t access);

/* ── CIPSO / netlabel ───────────────────────────────────────────── */

/* The default CIPSO DOI (Domain of Interpretation) used by Smack. */
#define SMACK_CIPSO_DOI 3

/* Bind a Smack label to a CIPSO tag (category) number.  Returns 0 on
 * success, -EINVAL on a bad label/tag, -ENOSPC if the tag table is full. */
int smack_cipso_map_label(const char *label, uint8_t tag);

/* Return the CIPSO category tag bound to a label, or -1 if unmapped. */
int smack_cipso_get_tag(const char *label);

/* Resolve a CIPSO tag back to its Smack label.  Returns 0 with the label
 * filled in (truncated to label_len), -ENOENT if the tag is unmapped. */
int smack_cipso_tag_to_label(uint8_t tag, char *label, int label_len);

/* ── Sysfs interface ────────────────────────────────────────────── */

/* Create /sys/fs/smackfs/ entries (called from smack_init) */
void smack_sysfs_init(void);

/* Default SMACK label for unlabeled subjects/objects */
#define SMACK_LABEL_FLOOR   "_"
#define SMACK_LABEL_STAR    "*"
#define SMACK_LABEL_HAT     "^"

#endif /* SMACK_H */
