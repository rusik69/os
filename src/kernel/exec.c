/* exec.c — Enhanced exec: secure exec, AT_SECURE auxv, cred switching
 *
 * Provides the do_execve() implementation that loads a new binary
 * image, switching credentials and setting up the auxiliary vector
 * with AT_SECURE and other modern fields.
 */

#include "types.h"
#include "elf.h"
#include "process.h"
#include "vmm.h"
#include "printf.h"
#include "string.h"
#include "errno.h"
#include "signal.h"
#include "vfs.h"
#include "caps.h"
#include "scheduler.h"
#include "uaccess.h"
#include "heap.h"

#define ELF_MAX_SIZE (1024 * 1024)
#define ELF_AUXV_ENTRIES   32

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

static int compute_at_secure(struct process *p)
{
    if (!p) return 0;
    if (p->uid != p->euid) return 1;
    if (p->gid != p->egid) return 1;
    if (p->securebits) return 1;
    return 0;
}

/* ── Yama ptrace scope check ────────────────────────────────────────── */

int yama_ptrace_scope_check(struct process *tracer, struct process *tracee)
{
    if (!tracer || !tracee)
        return -EINVAL;

    if (tracer == tracee || tracer->pid == tracee->pid)
        return 0;

    if (tracer->uid == 0 || tracer->euid == 0)
        return 0;

    if (tracer->uid == tracee->uid || tracer->euid == tracee->euid)
        return 0;

    struct process *ancestor = tracee;
    int max_depth = 64;
    while (ancestor && max_depth-- > 0) {
        if (ancestor->parent_pid == tracer->pid)
            return 0;
        int found = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            struct process *proc = process_get(i);
            if (proc && proc->pid == ancestor->parent_pid) {
                ancestor = proc;
                found = 1;
                break;
            }
        }
        if (!found) break;
    }
    return -EPERM;
}

static int setup_auxv(uint64_t *auxv_base, struct process *p,
                       uint64_t phdr, uint64_t phent, uint64_t phnum,
                       uint64_t entry, uint64_t base,
                       const char *filename)
{
    int i = 0;
    uint64_t *av = auxv_base;

    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_PHDR;    av[i++] = phdr; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_PHENT;   av[i++] = phent; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_PHNUM;   av[i++] = phnum; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_PAGESZ;  av[i++] = 4096; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_BASE;    av[i++] = base; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_FLAGS;   av[i++] = 0; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_ENTRY;   av[i++] = entry; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_UID;     av[i++] = p->uid; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_EUID;    av[i++] = p->euid; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_GID;     av[i++] = p->gid; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_EGID;    av[i++] = p->egid; }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_SECURE;  av[i++] = compute_at_secure(p); }
    if (i < ELF_AUXV_ENTRIES) { av[i++] = AT_CLKTCK;  av[i++] = 100; }
    if (i < ELF_AUXV_ENTRIES) {
        av[i++] = AT_RANDOM;
        av[i++] = (uint64_t)p->kernel_stack + 4000;
    }
    if (i < ELF_AUXV_ENTRIES && filename) {
        av[i++] = AT_EXECFN;
        av[i++] = (uint64_t)filename;
    }
    if (i + 1 < ELF_AUXV_ENTRIES) {
        av[i++] = AT_NULL;
        av[i++] = 0;
    }
    return i;
}

int do_execve(const char *filename, const char **argv, const char **envp)
{
    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    /* Read the binary file */
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) return -ENOMEM;

    uint32_t size = 0;
    if (vfs_read(filename, buf, ELF_MAX_SIZE, &size) < 0) {
        kfree(buf);
        return -ENOENT;
    }

    /* Get file stat for SUID/SGID/ownership checks */
    struct vfs_stat st;
    int has_stat = (vfs_stat(filename, &st) == 0);

    int has_setuid = has_stat && (st.mode & S_ISUID) ? 1 : 0;
    int has_setgid = has_stat && (st.mode & S_ISGID) ? 1 : 0;

    /* Switch credentials based on SUID/SGID */
    if (has_setuid && has_stat) {
        p->euid = st.uid;
    }
    if (has_setgid && has_stat) {
        p->egid = st.gid;
    }
    if (compute_at_secure(p)) {
        p->ngroups = 0;
    }

    /* Clear child TID */
    if (p->clear_child_tid) {
        copy_to_user((uint64_t)p->clear_child_tid, "\x00\x00\x00\x00", 4);
        p->clear_child_tid = NULL;
    }

    /* Load ELF */
    uint64_t entry = elf_load(buf, (uint64_t)size);
    kfree(buf);
    if (!entry) return -ENOEXEC;

    /* Use existing process_execve logic for full setup */
    return process_execve(filename, (char *const *)argv, (char *const *)envp);
}

int do_execveat(int dirfd, const char *pathname,
                 const char **argv, const char **envp,
                 int flags)
{
    (void)flags;
    char full_path[256];

    if (dirfd != AT_FDCWD) {
        struct process *p = process_get_current();
        if (!p) return -EINVAL;
        int i = dirfd - 3;
        if (i < 0 || i >= PROCESS_FD_MAX || !p->fd_table[i].used)
            return -EBADF;
        int plen = (int)strlen(p->fd_table[i].path);
        if (plen + 1 + (int)strlen(pathname) >= (int)sizeof(full_path))
            return -ENAMETOOLONG;
        memcpy(full_path, p->fd_table[i].path, (size_t)plen);
        if (full_path[plen - 1] != '/')
            full_path[plen++] = '/';
        memcpy(full_path + plen, pathname, strlen(pathname) + 1);
    } else {
        strncpy(full_path, pathname, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    }

    return do_execve(full_path, argv, envp);
}

void exec_init(void)
{
    kprintf("[OK] EXEC: enhanced exec with AT_SECURE, cred switching\n");
}

/* ── exec_mmap ────────────────────────────────────────────────────────── */
/*
 * Replace the current process's memory map (VMM space) with a new one
 * during exec. Clears existing user mappings and prepares for a new
 * ELF binary load.
 *
 * Returns 0 on success, negative on error.
 */
int exec_mmap(void *mm)
{
    (void)mm;

    struct process *p = process_get_current();
    if (!p)
        return -EINVAL;

    /* If the process already has a user PML4, we could destroy it here.
     * In a full implementation, we would:
     *   1. Save any relevant state from the old mm
     *   2. Destroy the old user page table
     *   3. Create a fresh page table for the new executable
     *
     * For now, we rely on elf_load() and vmm_setup_user_stack() to
     * create the appropriate mappings. */
    if (p->pml4) {
        /* Destroy old user page table to prevent leaking mappings */
        vmm_destroy_user_pml4(p->pml4);
        p->pml4 = NULL;
    }

    /* Create a fresh user PML4 for the new executable */
    p->pml4 = vmm_create_user_pml4();
    if (!p->pml4)
        return -ENOMEM;

    return 0;
}

/* ── exec_setup_stack ───────────────────────────────────────────────── */
/*
 * Set up the initial user stack for a new executable.
 * Allocates a stack region and populates it with argv, envp, auxv.
 *
 * @bprm: Binary parameters (contains argv/envp).
 * @sp:   Output: the initial stack pointer for the new process.
 *
 * Returns 0 on success, negative on error.
 */
int exec_setup_stack(void *bprm, uint64_t *sp)
{
    if (!bprm || !sp)
        return -EINVAL;

    struct process *p = process_get_current();
    if (!p)
        return -EINVAL;

    if (!p->pml4) {
        p->pml4 = vmm_create_user_pml4();
        if (!p->pml4)
            return -ENOMEM;
    }

    /* Allocate a user stack region at the top of user space.
     * A real implementation would map pages with proper permissions. */
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;

    /* Map the initial stack pages */
    for (uint64_t vaddr = stack_base; vaddr < USER_STACK_TOP; vaddr += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_zero_frame();
        if (!phys)
            return -ENOMEM;

        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER;
        if (vmm_map_user_page(p->pml4, vaddr, phys, flags) < 0) {
            return -ENOMEM;
        }
    }

    /* Set stack pointers */
    *sp = USER_STACK_TOP;
    p->user_stack_top = USER_STACK_TOP;
    p->user_stack_bottom = stack_base;

    /* Store stack area for AT_RANDOM (16 random bytes) */
    uint64_t random_area = USER_STACK_TOP - 32;
    uint64_t random_val = timer_get_ticks() ^ (uint64_t)(uintptr_t)p;
    uint8_t rand_bytes[16];
    for (int i = 0; i < 16; i++) {
        rand_bytes[i] = (uint8_t)((random_val >> (i * 4)) & 0xFF);
        rand_bytes[i] ^= (uint8_t)((uint64_t)(uintptr_t)&rand_bytes[i] >> (i * 2));
    }
    /* Copy random bytes to user stack */
    copy_to_user(random_area, rand_bytes, 16);

    return 0;
}

/* ── exec_binprm ─────────────────────────────────────────────────────── */
/*
 * Load a binary and set up execution parameters (bprm).
 * This function handles the full binary loading sequence:
 *   1. Open the binary file
 *   2. Set up bprm structure with argv/envp
 *   3. Call the ELF loader
 *   4. Set up the auxiliary vector
 *   5. Return the entry point and initial stack
 *
 * Returns 0 on success, negative on error.
 */
int exec_binprm(const char *filename, void *argv, void *envp)
{
    if (!filename)
        return -EINVAL;

    struct process *p = process_get_current();
    if (!p)
        return -EINVAL;

    /* Read the binary file */
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) return -ENOMEM;

    uint32_t size = 0;
    if (vfs_read(filename, buf, ELF_MAX_SIZE, &size) < 0) {
        kfree(buf);
        return -ENOENT;
    }

    /* Get file stat for permissions/SUID/SGID checks */
    struct vfs_stat st;
    int has_stat = (vfs_stat(filename, &st) == 0);
    if (!has_stat) { kfree(buf); return -EACCES; }

    /* Execute the IMA measurement hook */
    ima_measure(filename, IMA_FILE_EXEC);

    /* Execute the IPE policy check */
    int ipe_ret = ipe_check_exec(filename);
    if (ipe_ret != 0) { kfree(buf); return ipe_ret; }

    int has_setuid = (st.mode & S_ISUID) ? 1 : 0;
    int has_setgid = (st.mode & S_ISGID) ? 1 : 0;

    /* Load the ELF binary */
    uint64_t entry_point = elf_load(buf, (uint64_t)size);
    kfree(buf);
    if (!entry_point) return -ENOEXEC;

    /* Swap credentials if set-user/group-ID */
    if (has_setuid) p->euid = st.uid;
    if (has_setgid) p->egid = st.gid;

    /* Use process_execve for full stack/argv/envp setup */
    return process_execve(filename, (char *const *)argv, (char *const *)envp);

    kprintf("[exec] Loaded '%s' entry=0x%lx stack=0x%lx\n",
            filename, entry_point, user_stack);

    return 0;
}
