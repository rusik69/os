#ifndef IPC_NAMESPACE_H
#define IPC_NAMESPACE_H

/*
 * ipc_namespace.h — IPC namespace isolation
 *
 * Isolates System V IPC resources (semaphores, shared memory,
 * message queues) per namespace.  Each IPC namespace has its own
 * static arrays for semaphores (max 8), SHM segments (max 8),
 * and message queues (max 8).
 *
 * The initial namespace (init_ipc_ns) wraps the existing global
 * IPC tables.  Child namespaces allocate new isolated tables.
 */

#include "types.h"

/* ── Limits ──────────────────────────────────────────────────────── */
#define IPC_NS_MAX_NS       32   /* maximum number of IPC namespaces */
#define IPC_NS_MAX_SEMS      8   /* max semaphores per ns */
#define IPC_NS_MAX_SHM       8   /* max SHM segments per ns */
#define IPC_NS_MAX_MSG       8   /* max message queues per ns */

/* ── Semaphore set (simplified System V sem) ────────────────────── */
struct ipc_ns_sem {
    int     used;
    int     key;
    int     semval;        /* single value (simplified) */
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
};

/* ── Shared memory segment ──────────────────────────────────────── */
struct ipc_ns_shm {
    int     used;
    int     key;
    uint64_t phys;         /* physical frame address */
    int     refs;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
};

/* ── Message queue ──────────────────────────────────────────────── */
struct ipc_ns_msg {
    int     used;
    int     key;
    char    data[64];      /* simple fixed-size message */
    uint32_t len;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
};

/* ── IPC namespace descriptor ───────────────────────────────────── */
struct ipc_namespace {
    int     id;
    int     in_use;

    /* Semaphore table */
    int     sem_count;
    struct ipc_ns_sem sem_table[IPC_NS_MAX_SEMS];

    /* Shared memory table */
    int     shm_count;
    struct ipc_ns_shm shm_table[IPC_NS_MAX_SHM];

    /* Message queue table */
    int     msg_count;
    struct ipc_ns_msg msg_table[IPC_NS_MAX_MSG];
};

/* ── Root (initial) IPC namespace ───────────────────────────────── */
extern struct ipc_namespace init_ipc_ns;

/* ── API ────────────────────────────────────────────────────────── */

/* Initialize the root IPC namespace (called once at boot) */
void ipc_ns_init(void);

/* Create a new IPC namespace (isolated copy of current state).
 * Returns NULL on failure. */
struct ipc_namespace *ipc_ns_create(void);

/* Free/release an IPC namespace. */
void ipc_ns_free(struct ipc_namespace *ns);

/* Get the current process's IPC namespace */
struct ipc_namespace *ipc_ns_current(void);

/* ── System V semaphore wrappers ────────────────────────────────── */

/* semget: create or access a semaphore set */
int ipc_ns_semget(struct ipc_namespace *ns, int key, int nsems, int semflg);

/* semop: perform semaphore operations */
int ipc_ns_semop(struct ipc_namespace *ns, int semid,
                 const void *sops, unsigned int nsops);

/* ── System V shared memory wrappers ────────────────────────────── */

/* shmget: create or access a shared memory segment */
int ipc_ns_shmget(struct ipc_namespace *ns, int key, size_t size, int shmflg);

/* shmat: attach shared memory */
void *ipc_ns_shmat(struct ipc_namespace *ns, int shmid, const void *shmaddr, int shmflg);

/* shmdt: detach shared memory */
int ipc_ns_shmdt(struct ipc_namespace *ns, const void *shmaddr);

/* ── System V message queue wrappers ────────────────────────────── */

/* msgget: create or access a message queue */
int ipc_ns_msgget(struct ipc_namespace *ns, int key, int msgflg);

/* msgsnd: send a message */
int ipc_ns_msgsnd(struct ipc_namespace *ns, int msqid,
                  const void *msgp, size_t msgsz, int msgflg);

/* msgrcv: receive a message */
ssize_t ipc_ns_msgrcv(struct ipc_namespace *ns, int msqid,
                       void *msgp, size_t msgsz, long msgtyp, int msgflg);

#endif /* IPC_NAMESPACE_H */
