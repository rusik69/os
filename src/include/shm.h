#ifndef SHM_H
#define SHM_H

#include "types.h"

/* ── Permission bits (Unix-style rwxrwxrwx) ────────────────────────── */
#define SHM_R           00400   /* owner read  */
#define SHM_W           00200   /* owner write */
#define SHM_RW          00600   /* owner read-write (default for new segments) */

#define SHM_GRP_R       00040   /* group read  */
#define SHM_GRP_W       00020   /* group write */
#define SHM_GRP_RW      00060   /* group read-write */

#define SHM_OTH_R       00004   /* other read  */
#define SHM_OTH_W       00002   /* other write */
#define SHM_OTH_RW      00006   /* other read-write */

#define SHM_MODE_MASK   00777   /* all permission bits */

/* ── shmctl() commands ─────────────────────────────────────────────── */
#define SHMCTL_IPC_STAT  1     /* get segment metadata (perms, uid, gid) */
#define SHMCTL_IPC_SET   2     /* set segment owner / permissions */
#define SHMCTL_IPC_RMID  3     /* remove segment (alias for shm_free) */

/* ── SHM metadata structure (returned by SHMCTL_IPC_STAT) ──────────── */
struct shm_perm {
    int      id;               /* segment id */
    int      key;              /* System V key */
    uint32_t uid;              /* owner UID */
    uint32_t gid;              /* owner GID */
    uint16_t mode;             /* permission bits */
    int      refs;             /* attach count */
    uint64_t phys;             /* physical frame (for debugging) */
    int      used;             /* 1 if slot is active */
};

/* ── API ───────────────────────────────────────────────────────────── */
void     shm_init(void);
int      shm_get(int key, uint16_t mode);  /* get/create segment → id or -1 */
uint64_t shm_at(int id);                   /* map into current process → virt addr or 0 */
int      shm_dt(int id);                   /* decrement ref count (always allowed) */
int      shm_free(int id);                 /* free segment (owner/root only) */
int      shm_perm_set(int id, uint32_t uid, uint32_t gid, uint16_t mode);
int      shm_perm_get(int id, struct shm_perm *out);

#endif
