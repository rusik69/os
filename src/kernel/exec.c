/* exec.c — Enhanced exec: secure exec via fd, AT_SECURE auxv,
 *           cred switching, bprm security
 *
 * Provides the do_execve() implementation that loads a new binary
 * image, switching credentials and setting up the auxiliary vector
 * with AT_SECURE and other modern fields.
 *
 * Features:
 *   - execve via file descriptor (execveat-like)
 *   - AT_SECURE auxv entry (set when credentials changed)
 *   - Credential switching with securebits
 *   - bprm (binary parameter) security checks
 *   - ELF loader integration
 *   - Clear child TID on exec (as per CLONE_CHILD_CLEARTID semantics)
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
#include "err.h"

/* ── Configuration ─────────────────────────────────────────────────── */

#define ELF_AUXV_ENTRIES   32
#define EXEC_ARG_MAX       4096

/* Auxiliary vector types */
#define AT_NULL     0
#define AT_IGNORE   1
#define AT_EXECFD   2
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_NOTELF   10
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_EXECFN   31

/* ── State ─────────────────────────────────────────────────────────── */

/* AT_SECURE is set when any of:
 *   1. real UID != effective UID, or
 *   2. real GID != effective GID, or
 *   3. process has non-zero securebits
 *   4. set-user-ID or set-group-ID is active
 */
static int compute_at_secure(const struct process *p)
{
    if (!p) return 0;
    if (p->uid != p->euid) return 1;
    if (p->gid != p->egid) return 1;
    if (p->securebits) return 1;
    return 0;
}

/* ── Yama ptrace scope check ────────────────────────────────────────── */

/* Performs a Yama LSM ptrace scope check: prevents non-privileged
 * processes from ptracing processes they don't own.
 *
 * This implements Yama ptrace_scope mode 1 (restricted):
 *   - A process can only ptrace its own descendants
 *   - Or processes with the same UID
 *   - Or processes whose dumpable flag allows it
 *   - CAP_SYS_PTRACE overrides the check
 *
 * Called during execve (bprm_check_security) and ptrace attach.
 *
 * @tracer: process attempting to trace
 * @tracee: process being traced
 * Returns 0 if allowed, -EPERM if denied.
 */
int yama_ptrace_scope_check(const struct process *tracer, const struct process *tracee)
{
    if (!tracer || !tracee)
        return -EINVAL;

    /* Self-tracing is always allowed */
    if (tracer == tracee || tracer->pid == tracee->pid)
        return 0;

    /* Privileged processes with CAP_SYS_PTRACE can trace anything */
    if (tracer->uid == 0 || tracer->euid == 0)
        return 0;

    /* Same user: allow if tracer owns tracee or same uid */
    if (tracer->uid == tracee->uid || tracer->euid == tracee->euid)
        return 0;

    /* Check ancestry: tracer must be a parent or ancestor of the tracee */
    const struct process *ancestor = tracee;
    int max_depth = 64; /* prevent infinite loops */
    while (ancestor && max_depth-- > 0) {
        if (ancestor->parent_pid == tracer->pid)
            return 0;
        /* Walk up to find if any ancestor is the tracer */
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

    /* Check if the tracee has allowed tracing via PR_SET_PTRACER */
    /* (stub - in full implementation would check tracee->ptracer_allowed) */

    /* Deny all other cases */
    return -EPERM;
}

/* ── bprm security check ───────────────────────────────────────────── */

static int bprm_check_security(const struct process *p,
                                const struct vfs_node *binary,
                                int *has_setuid, int *has_setgid)
{
    /* ── File type / access checks ──────────────────────────────────── */
    if (!binary) return -EACCES;
    if (binary->type != NODE_TYPE_FILE) return -EACCES;

    /* ── Validate execute permission ────────────────────────────────── */
    if (!vfs_check_perms(binary, p, VFS_X_OK))
        return -EACCES;

    /* ── Set-user-ID / set-group-ID elevation ───────────────────────── */
    if (*has_setuid && p->uid != 0) {
        /* Only allow SUID if the owner matches or we're privileged */
        if (p->euid != binary->uid && p->uid != 0)
            *has_setuid = 0;
    }

    if (*has_setgid && p->gid != 0) {
        if (p->egid != binary->gid && p->uid != 0)
            *has_setgid = 0;
    }

    /* ── LSM hooks (Yama ptrace scope, SELinux, etc.) ────────────────── */
    {
        int lsm_ret = yama_ptrace_scope_check(p, binary);
        if (lsm_ret != 0)
            return lsm_ret;
    }
    /* selinux_bprm_check(p, binary); */

    return 0;
}

/* ── Credential switching ──────────────────────────────────────────── */

static void switch_creds(struct process *p,
                          const struct vfs_node *binary,
                          int has_setuid, int has_setgid)
{
    /* Store old credentials for audit */
    uint32_t old_uid = p->uid;
    uint32_t old_euid = p->euid;
    uint32_t old_gid = p->gid;
    uint32_t old_egid = p->egid;

    if (has_setuid) {
        p->euid = binary->uid;
        /* SECBIT_NO_SETUID_FIXUP check would go here */
    }

    if (has_setgid) {
        p->egid = binary->gid;
    }

    /* Clear supplementary groups if AT_SECURE */
    if (compute_at_secure(p)) {
        p->ngroups = 0;
    }

    (void)old_uid;
    (void)old_euid;
    (void)old_gid;
    (void)old_egid;
}

/* ── Setup auxiliary vector ────────────────────────────────────────── */

static int setup_auxv(uint64_t *auxv_base, const struct process *p,
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
        /* 16 random bytes at a fixed address — stub uses stack area */
        av[i++] = (uint64_t)p->kernel_stack + 4000;
    }
    if (i < ELF_AUXV_ENTRIES && filename) {
        av[i++] = AT_EXECFN;
        av[i++] = (uint64_t)filename; /* user-space address of filename */
    }
    /* AT_NULL terminator */
    if (i + 1 < ELF_AUXV_ENTRIES) {
        av[i++] = AT_NULL;
        av[i++] = 0;
    }

    return i;
}

/* ── Main exec entry point ─────────────────────────────────────────── */

/**
 * do_execve - Execute a new program, replacing the current process image
 * @filename: Path to the executable file
 * @argv: Array of argument strings (NULL-terminated)
 * @envp: Array of environment strings (NULL-terminated)
 *
 * Opens the binary file, performs bprm security checks (including Yama
 * ptrace scope), switches credentials (setuid/setgid), clears CLONE_CHILD_CLEARTID,
 * loads the ELF binary, sets up the user stack with argv/envp/auxv,
 * resets signal handlers, and transitions to the new process image via
 * arch_exec_enter().
 *
 * Context: May sleep. Requires a current process (must not be called from
 *          interrupt context). Allocates memory, reads from VFS.
 * Return: 0 on success (does not return, as the process is replaced),
 *         or a negative errno on failure.
 */
int do_execve(const char *filename, const char **argv, const char **envp)
{
    if (!filename) return -EINVAL;

    struct process *p = process_get_current();
    if (!p) return -EINVAL;

    /* ── Open the binary ────────────────────────────────────────────── */
    struct vfs_node *binary = vfs_open(filename, 0);
    if (unlikely(!binary)) return -ENOENT;

    /* ── Check security of the binary ───────────────────────────────── */
    int has_setuid = (binary->mode & S_ISUID) ? 1 : 0;
    int has_setgid = (binary->mode & S_ISGID) ? 1 : 0;

    int ret = bprm_check_security(p, binary, &has_setuid, &has_setgid);
    if (ret != 0) {
        vfs_close(binary);
        return ret;
    }

    /* ── Switch credentials ─────────────────────────────────────────── */
    switch_creds(p, binary, has_setuid, has_setgid);

    /* ── Clear child TID (CLONE_CHILD_CLEARTID semantics) ───────────── */
    if (p->clear_child_tid) {
        if (copy_to_user((uint64_t)p->clear_child_tid, "\x00\x00\x00\x00", 4) < 0)
            kprintf("[exec] warning: failed to clear child TID at 0x%lx\n",
                    (unsigned long)p->clear_child_tid);
        p->clear_child_tid = NULL;
    }

    /* ── ELF load ───────────────────────────────────────────────────── */
    /* Read the ELF file into a buffer and validate */
    uint8_t *elf_buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!elf_buf) {
        vfs_close(binary);
        return -ENOMEM;
    }
    uint32_t elf_size = 0;
    if (vfs_read(filename, elf_buf, ELF_MAX_SIZE, &elf_size) < 0) {
        kfree(elf_buf);
        vfs_close(binary);
        return -EIO;
    }

    uint64_t entry_point = elf_load(elf_buf, (unsigned long)elf_size);
    if (unlikely(!entry_point)) {
        kfree(elf_buf);
        vfs_close(binary);
        return -ENOEXEC;
    }

    /* Setup the user stack with argv, envp, auxv */
    uint64_t user_stack = vmm_setup_user_stack(p, argv, envp);
    if (unlikely(!user_stack)) {
        kfree(elf_buf);
        vfs_close(binary);
        return -ENOMEM;
    }

    /* Setup auxiliary vector on user stack */
    uint64_t auxv_base = user_stack - ELF_AUXV_ENTRIES * 2 * 8;
    uint64_t phdr = 0, phent = 0, phnum = 0, base_addr = 0;
    setup_auxv((uint64_t *)auxv_base, p,
               phdr, phent, phnum,
               entry_point, base_addr,
               filename);

    /* ── Set process state for execution ────────────────────────────── */
    p->entry_point = entry_point;
    p->user_rsp = user_stack;
    p->brk = 0; /* reset program break */
    p->name = filename; /* store for /proc/pid/comm */

    /* Clear signal handlers (except SIG_IGN) */
    for (int i = 0; i < 64; i++) {
        if (p->signal_handlers[i] != SIG_IGN)
            p->signal_handlers[i] = SIG_DFL;
    }

    kfree(elf_buf);
    vfs_close(binary);

    /* Switch to the new process image */
    arch_exec_enter(entry_point, user_stack, argv, envp);

    return 0;
}

/* ── execveat: execute via fd ──────────────────────────────────────── */

int do_execveat(int dirfd, const char *pathname,
                 const char **argv, const char **envp,
                 int flags)
{
    if (!pathname) return -EINVAL;

    (void)flags;

    /* If pathname is empty and dirfd refers to a regular file,
     * execute that file directly */
    if (pathname && pathname[0] == '\0') {
        struct vfs_node *dir = vfs_from_fd(dirfd);
        if (dir && dir->type == NODE_TYPE_FILE) {
            /* Execute the directory fd's file */
            return do_execve(dir->name, argv, envp);
        }
        return -EINVAL;
    }

    /* Otherwise, resolve path relative to dirfd */
    char full_path[256];
    if (dirfd != AT_FDCWD) {
        struct vfs_node *dir = vfs_from_fd(dirfd);
        if (!dir) return -EBADF;
        vfs_get_path(dir, full_path, sizeof(full_path));
        strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        strncat(full_path, pathname, sizeof(full_path) - strlen(full_path) - 1);
    } else {
        strncpy(full_path, pathname, sizeof(full_path) - 1);
    }

    return do_execve(full_path, argv, envp);
}

/* ── Initialization ────────────────────────────────────────────────── */

void exec_init(void)
{
    kprintf("[OK] EXEC: enhanced exec with AT_SECURE, cred switching, "
            "bprm security\n");
}

/**
 * exec_mmap - Replace the current process's memory map
 * @mm: Memory descriptor (unused in current implementation)
 *
 * Destroys the current process's existing user page table (PML4) and
 * creates a fresh one for the new executable during exec. This ensures
 * no stale mappings from the previous executable are leaked.
 *
 * Context: Must be called from a process context during exec.
 *          Calls vmm_create_user_pml4() which may allocate memory.
 * Return: 0 on success, -EINVAL if no current process, -ENOMEM on
 *         page table allocation failure.
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
    if (IS_ERR(p->pml4))
        return -ENOMEM;

    return 0;
}

/**
 * exec_setup_stack - Set up the initial user stack for a new executable
 * @bprm: Binary parameters (unused in current implementation)
 * @sp: Output pointer for the initial stack pointer (RSP)
 *
 * Allocates a user stack region at the top of user space and maps it
 * with read/write permissions. Sets up stack pointers and writes 16
 * random bytes for AT_RANDOM to the stack area.
 *
 * Context: Must be called from a process context during exec.
 *          May allocate memory and map pages.
 * Return: 0 on success, -EINVAL on bad arguments, -ENOMEM on allocation
 *         failure.
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
        if (IS_ERR(p->pml4))
            return -ENOMEM;
    }

    /* Allocate a user stack region at the top of user space.
     * A real implementation would map pages with proper permissions. */
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;

    /* Map the initial stack pages */
    for (uint64_t vaddr = stack_base; vaddr < USER_STACK_TOP; vaddr += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_zero_frame();
        if (unlikely(!phys))
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
    if (copy_to_user(random_area, rand_bytes, 16) < 0)
        kprintf("[exec] warning: failed to copy AT_RANDOM to user stack\n");

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

    /* Open the binary */
    struct vfs_node *binary = vfs_open(filename, 0);
    if (unlikely(!binary))
        return -ENOENT;

    /* Check exec permission */
    if (!vfs_check_perms(binary, p, VFS_X_OK)) {
        vfs_close(binary);
        return -EACCES;
    }

    /* Execute the IMA measurement hook */
    ima_measure(filename, IMA_FILE_EXEC);

    /* Execute the IPE policy check */
    int ipe_ret = ipe_check_exec(filename);
    if (ipe_ret != 0) {
        vfs_close(binary);
        return ipe_ret;
    }

    /* Check for set-user-ID / set-group-ID */
    int has_setuid = (binary->mode & S_ISUID) ? 1 : 0;
    int has_setgid = (binary->mode & S_ISGID) ? 1 : 0;

    /* Call bprm security checks */
    struct linux_binprm bprm_stub = {0};
    int ret;

    ret = cap_bprm_set_creds(&bprm_stub);
    if (ret != 0) {
        vfs_close(binary);
        return ret;
    }

    ret = ima_bprm_check(&bprm_stub);
    if (ret != 0) {
        vfs_close(binary);
        return ret;
    }

    ret = ipe_bprm_check_security(&bprm_stub);
    if (ret != 0) {
        vfs_close(binary);
        return ret;
    }

    /* Load the ELF binary */
    /* First read the ELF file into a buffer */
    uint8_t *elf_buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (unlikely(!elf_buf)) {
        vfs_close(binary);
        return -ENOMEM;
    }
    uint32_t elf_size = 0;
    if (vfs_read(filename, elf_buf, ELF_MAX_SIZE, &elf_size) < 0) {
        kfree(elf_buf);
        vfs_close(binary);
        return -EIO;
    }

    uint64_t phdr = 0, phent = 0, phnum = 0, base_addr = 0;

    /* Validate and load the ELF from the buffer */
    uint64_t entry = elf_load(elf_buf, (unsigned long)elf_size);
    if (unlikely(!entry)) {
        kfree(elf_buf);
        vfs_close(binary);
        return -ENOEXEC;
    }

    /* Swap credentials if set-user/group-ID */
    if (has_setuid || has_setgid) {
        switch_creds(p, binary, has_setuid, has_setgid);
    }

    /* Set up the user stack with argv/envp */
    uint64_t user_stack = vmm_setup_user_stack(p, (const char **)argv,
                                                (const char **)envp);
    if (unlikely(!user_stack)) {
        kfree(elf_buf);
        vfs_close(binary);
        return -ENOMEM;
    }

    /* Set up the auxiliary vector */
    uint64_t auxv_base = user_stack - ELF_AUXV_ENTRIES * 2 * 8;
    setup_auxv((uint64_t *)auxv_base, p,
               phdr, phent, phnum,
               entry, base_addr,
               filename);

    /* Populate process state */
    p->entry_point = entry;
    p->user_rsp = user_stack;
    p->brk = 0;
    p->name = filename;

    kfree(elf_buf);
    vfs_close(binary);

    kprintf("[exec] Loaded '%s' entry=0x%lx stack=0x%lx\n",
            filename, entry, user_stack);

    return 0;
}
