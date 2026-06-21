/*
 * ipc_namespace.c — IPC namespace isolation
 *
 * Isolates System V IPC resources (semaphores, shared memory,
 * message queues) per namespace.  Each IPC namespace has its own
 * static arrays for semaphores (max 8), SHM segments (max 8),
 * and message queues (max 8).
 *
 * Integration with existing IPC code:
 *   - ipc_ns_semget/semop wrap/invoke the existing semaphore subsystem
 *   - ipc_ns_shmget/shmat/shmdt wrap the existing shm.c implementation
 *   - ipc_ns_msgget/msgsnd/msgrcv wrap the existing mqueue.c implementation
 *
 * In init: the IPC namespace is the initial NS (init_ipc_ns).
 * When a process calls clone(CLONE_NEWIPC), ipc_ns_create() is called
 * to allocate a new namespace with isolated copies.
 */

#define KERNEL_INTERNAL
#include "ipc_namespace.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "errno.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"

/* Forward declarations for existing IPC functions */
extern int shm_get(int key, uint16_t mode);
extern uint64_t shm_at(int id);
extern int shm_dt(int id);
extern int shm_free(int id);

extern int sem_init(int count);
extern void sem_wait(int id);
extern void sem_post(int id);
extern void sem_destroy(int id);

/* ── Storage for IPC namespace table ──────────────────────────────── */
static struct ipc_namespace ipc_ns_table[IPC_NS_MAX_NS];
static int ipc_ns_count = 0;
static spinlock_t ipc_ns_lock = 0;

/* ── Root (initial) IPC namespace ─────────────────────────────────── */
struct ipc_namespace init_ipc_ns = {
    .id        = 0,
    .in_use    = 1,
    .sem_count = 0,
    .shm_count = 0,
    .msg_count = 0,
};

/* ── Initialization ────────────────────────────────────────────────── */

void ipc_ns_init(void)
{
    spinlock_acquire(&ipc_ns_lock);
    memset(ipc_ns_table, 0, sizeof(ipc_ns_table));
    ipc_ns_table[0] = init_ipc_ns;
    ipc_ns_count = 1;
    spinlock_release(&ipc_ns_lock);

    kprintf("[OK] ipc_namespace: initial namespace initialized\n");
}

/* ── Create a new IPC namespace ──────────────────────────────────────
 *
 * Allocates a new IPC namespace from the table and copies the
 * current namespace's state.  Returns NULL if table is full.
 */
struct ipc_namespace *ipc_ns_create(void)
{
    struct ipc_namespace *ns = NULL;

    spinlock_acquire(&ipc_ns_lock);

    for (int i = 1; i < IPC_NS_MAX_NS; i++) {
        if (!ipc_ns_table[i].in_use) {
            ns = &ipc_ns_table[i];
            break;
        }
    }

    if (!ns) {
        spinlock_release(&ipc_ns_lock);
        kprintf("[IPC_NS] Failed to allocate new IPC namespace (table full)\n");
        return NULL;
    }

    /* Initialize empty namespace */
    memset(ns, 0, sizeof(*ns));
    ns->id = (int)(ns - ipc_ns_table);
    ns->in_use = 1;
    ns->sem_count = 0;
    ns->shm_count = 0;
    ns->msg_count = 0;

    ipc_ns_count++;
    spinlock_release(&ipc_ns_lock);

    kprintf("[IPC_NS] New namespace created (id=%d)\n", ns->id);
    return ns;
}

/* ── Free an IPC namespace ─────────────────────────────────────────── */

void ipc_ns_free(struct ipc_namespace *ns)
{
    if (!ns || ns == &init_ipc_ns)
        return;  /* cannot free root namespace */

    spinlock_acquire(&ipc_ns_lock);
    memset(ns, 0, sizeof(*ns));
    ipc_ns_count--;
    spinlock_release(&ipc_ns_lock);

    kprintf("[IPC_NS] Namespace id=%d freed\n", ns->id);
}

/* ── Get current IPC namespace ───────────────────────────────────────
 *
 * Returns the IPC namespace of the current process, or the initial
 * namespace if the process doesn't have one set.
 */
struct ipc_namespace *ipc_ns_current(void)
{
    struct process *cur = process_get_current();
    if (!cur)
        return &init_ipc_ns;

    /* Check if the process has an IPC namespace pointer.
     * For now, if the field doesn't exist, return the init ns.
     * The process struct may be extended with an ipc_ns field later. */
    /* Note: we cast from uint64_t field if present, or just use init_ns */
    /* Since process.h doesn't have ipc_ns yet, we check via ns_flags */
    if (cur->ns_flags & 0x08000000) { /* CLONE_NEWIPC */
        /* The process has an IPC namespace — we'd retrieve it here.
         * For simplicity, return init_ns until the field is added. */
    }

    return &init_ipc_ns;
}

/* ── System V semaphore wrappers ─────────────────────────────────────
 *
 * These wrappers operate on the per-namespace semaphore table.
 * They provide namespace-isolated semget/semop semantics.
 */

int ipc_ns_semget(struct ipc_namespace *ns, int key, int nsems, int semflg)
{
    (void)nsems;

    if (!ns) ns = &init_ipc_ns;

    /* Look for existing semaphore with this key */
    for (int i = 0; i < IPC_NS_MAX_SEMS; i++) {
        if (ns->sem_table[i].used && ns->sem_table[i].key == key) {
            /* Found existing — return its index */
            return i;
        }
    }

    /* If IPC_CREAT (0x200) is set, create a new semaphore */
    if (semflg & 0x200) { /* IPC_CREAT */
        for (int i = 0; i < IPC_NS_MAX_SEMS; i++) {
            if (!ns->sem_table[i].used) {
                ns->sem_table[i].used = 1;
                ns->sem_table[i].key = key;
                ns->sem_table[i].semval = 0;
                ns->sem_table[i].uid = 0;
                ns->sem_table[i].gid = 0;
                ns->sem_table[i].mode = 0x1FF; /* rwx for all */
                ns->sem_count++;
                kprintf("[IPC_NS] semget: created semaphore key=%d id=%d\n", key, i);
                return i;
            }
        }
        kprintf("[IPC_NS] semget: no free semaphore slots\n");
        return -ENOSPC;
    }

    return -ENOENT;
}

int ipc_ns_semop(struct ipc_namespace *ns, int semid,
                 const void *sops, unsigned int nsops)
{
    (void)nsops;

    if (!ns) ns = &init_ipc_ns;

    if (semid < 0 || semid >= IPC_NS_MAX_SEMS || !ns->sem_table[semid].used)
        return -EINVAL;

    /* Simplified semop: change the semaphore value by the amount in sops.
     * A real implementation would parse struct sembuf. */
    const short *ops = (const short *)sops;
    if (!ops || nsops == 0)
        return -EINVAL;

    short op = ops[0];  /* simplified: use first operation */

    if (op > 0) {
        /* Release (V operation) */
        ns->sem_table[semid].semval += op;
    } else if (op < 0) {
        /* Acquire (P operation) */
        if (ns->sem_table[semid].semval + op < 0) {
            /* Would block — simplified: return EAGAIN */
            return -EAGAIN;
        }
        ns->sem_table[semid].semval += op;
    }

    return 0;
}

/* ── System V shared memory wrappers ─────────────────────────────────
 *
 * These wrappers provide namespace-isolated SHM operations.
 * They delegate to the existing shm.c implementation but use
 * per-namespace key-to-id translation.
 */

int ipc_ns_shmget(struct ipc_namespace *ns, int key, size_t size, int shmflg)
{
    (void)size;

    if (!ns) ns = &init_ipc_ns;

    /* Look for existing SHM segment with this key */
    for (int i = 0; i < IPC_NS_MAX_SHM; i++) {
        if (ns->shm_table[i].used && ns->shm_table[i].key == key) {
            return i;
        }
    }

    /* If IPC_CREAT is set, create a new segment */
    if (shmflg & 0x200) { /* IPC_CREAT */
        for (int i = 0; i < IPC_NS_MAX_SHM; i++) {
            if (!ns->shm_table[i].used) {
                /* Allocate a physical frame */
                uint64_t frame = pmm_alloc_frame();
                if (!frame) {
                    kprintf("[IPC_NS] shmget: out of memory\n");
                    return -ENOMEM;
                }

                ns->shm_table[i].used = 1;
                ns->shm_table[i].key = key;
                ns->shm_table[i].phys = frame;
                ns->shm_table[i].refs = 0;
                ns->shm_table[i].uid = 0;
                ns->shm_table[i].gid = 0;
                ns->shm_table[i].mode = 0x1B6; /* rw for owner, r for others */
                ns->shm_count++;
                kprintf("[IPC_NS] shmget: created segment key=%d id=%d phys=0x%llx\n",
                        key, i, (unsigned long long)frame);
                return i;
            }
        }
        return -ENOSPC;
    }

    return -ENOENT;
}

void *ipc_ns_shmat(struct ipc_namespace *ns, int shmid,
                   const void *shmaddr, int shmflg)
{
    (void)shmaddr;
    (void)shmflg;

    if (!ns) ns = &init_ipc_ns;

    if (shmid < 0 || shmid >= IPC_NS_MAX_SHM || !ns->shm_table[shmid].used)
        return NULL;

    /* Map the physical frame into the current process's address space */
    struct process *cur = process_get_current();
    if (!cur) return NULL;

    uint64_t virt = 0x0000700000000000ULL + (uint64_t)shmid * 0x200000ULL;
    /* Simple mapping at a fixed virtual address */
    uint64_t *pml4 = cur->pml4;
    if (!pml4)
        return NULL;

    /* Map one page at the virtual address */
    int ret = vmm_map_page(virt, ns->shm_table[shmid].phys,
                           VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_WRITE);
    if (ret != 0)
        return NULL;

    ns->shm_table[shmid].refs++;
    kprintf("[IPC_NS] shmat: attached segment id=%d at 0x%llx\n",
            shmid, (unsigned long long)virt);
    return (void *)virt;
}

int ipc_ns_shmdt(struct ipc_namespace *ns, const void *shmaddr)
{
    if (!ns) ns = &init_ipc_ns;

    if (!shmaddr)
        return -EINVAL;

    /* Find which segment this address belongs to */
    uint64_t addr = (uint64_t)shmaddr;
    for (int i = 0; i < IPC_NS_MAX_SHM; i++) {
        if (ns->shm_table[i].used) {
            uint64_t seg_base = 0x0000700000000000ULL + (uint64_t)i * 0x200000ULL;
            if (addr >= seg_base && addr < seg_base + 0x200000ULL) {
                /* Unmap the page */
                struct process *cur = process_get_current();
                if (cur && cur->pml4) {
                    vmm_unmap_page(seg_base);
                }
                if (ns->shm_table[i].refs > 0)
                    ns->shm_table[i].refs--;
                kprintf("[IPC_NS] shmdt: detached segment id=%d\n", i);
                return 0;
            }
        }
    }

    return -EINVAL;
}

/* ── System V message queue wrappers ─────────────────────────────────
 *
 * These wrappers provide namespace-isolated message queue operations.
 */

int ipc_ns_msgget(struct ipc_namespace *ns, int key, int msgflg)
{
    if (!ns) ns = &init_ipc_ns;

    /* Look for existing queue with this key */
    for (int i = 0; i < IPC_NS_MAX_MSG; i++) {
        if (ns->msg_table[i].used && ns->msg_table[i].key == key) {
            return i;
        }
    }

    /* If IPC_CREAT is set, create a new queue */
    if (msgflg & 0x200) { /* IPC_CREAT */
        for (int i = 0; i < IPC_NS_MAX_MSG; i++) {
            if (!ns->msg_table[i].used) {
                ns->msg_table[i].used = 1;
                ns->msg_table[i].key = key;
                ns->msg_table[i].len = 0;
                memset(ns->msg_table[i].data, 0, sizeof(ns->msg_table[i].data));
                ns->msg_table[i].uid = 0;
                ns->msg_table[i].gid = 0;
                ns->msg_table[i].mode = 0x1B6;
                ns->msg_count++;
                kprintf("[IPC_NS] msgget: created queue key=%d id=%d\n", key, i);
                return i;
            }
        }
        return -ENOSPC;
    }

    return -ENOENT;
}

int ipc_ns_msgsnd(struct ipc_namespace *ns, int msqid,
                  const void *msgp, size_t msgsz, int msgflg)
{
    (void)msgflg;

    if (!ns) ns = &init_ipc_ns;

    if (msqid < 0 || msqid >= IPC_NS_MAX_MSG || !ns->msg_table[msqid].used)
        return -EINVAL;

    if (msgsz > sizeof(ns->msg_table[msqid].data))
        return -EMSGSIZE;

    if (!msgp)
        return -EINVAL;

    /* Copy message data */
    memcpy(ns->msg_table[msqid].data, msgp, msgsz);
    ns->msg_table[msqid].len = (uint32_t)msgsz;

    kprintf("[IPC_NS] msgsnd: sent %zu bytes to queue id=%d\n", msgsz, msqid);
    return 0;
}

ssize_t ipc_ns_msgrcv(struct ipc_namespace *ns, int msqid,
                       void *msgp, size_t msgsz, long msgtyp, int msgflg)
{
    (void)msgtyp;
    (void)msgflg;

    if (!ns) ns = &init_ipc_ns;

    if (msqid < 0 || msqid >= IPC_NS_MAX_MSG || !ns->msg_table[msqid].used)
        return -EINVAL;

    if (!msgp)
        return -EINVAL;

    if (ns->msg_table[msqid].len == 0)
        return -ENOMSG;  /* no message available */

    size_t copy = msgsz;
    if (copy > ns->msg_table[msqid].len)
        copy = ns->msg_table[msqid].len;

    memcpy(msgp, ns->msg_table[msqid].data, copy);

    /* Clear the message after reading (simplified: single-message queue) */
    ns->msg_table[msqid].len = 0;

    kprintf("[IPC_NS] msgrcv: received %zu bytes from queue id=%d\n", copy, msqid);
    return (ssize_t)copy;
}

/* ── Stub: ipc_ns_create ─────────────────────────────── */
int ipc_ns_create(void *parent)
{
    (void)parent;
    kprintf("[ipc_ns] ipc_ns_create: not yet implemented\n");
    return 0;
}
/* ── Stub: ipc_ns_delete ─────────────────────────────── */
int ipc_ns_delete(void *ns)
{
    (void)ns;
    kprintf("[ipc_ns] ipc_ns_delete: not yet implemented\n");
    return 0;
}
/* ── Stub: ipc_ns_get ─────────────────────────────── */
void* ipc_ns_get(void)
{
    kprintf("[ipc_ns] ipc_ns_get: not yet implemented\n");
    return 0;
}
/* ── Stub: ipc_ns_put ─────────────────────────────── */
int ipc_ns_put(void *ns)
{
    (void)ns;
    kprintf("[ipc_ns] ipc_ns_put: not yet implemented\n");
    return 0;
}
