#define KERNEL_INTERNAL
#include "elf.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "scheduler.h"
#include "aslr.h"
#include "auxv.h"
#include "err.h"
#include "signalfd.h"

/* Max ELF binary we'll try to load from disk */
#define ELF_MAX_SIZE (1024 * 1024)  /* 1MB — increased from 64KB to support real binaries */

/* Maximum number of program headers we'll process — bounds phnum array loop */
#define ELF_MAX_PHNUM  128

/**
 * elf_validate - Validate ELF headers and return entry point
 * @data: Pointer to the ELF file data in memory
 * @size: Size of the ELF file data
 * @out_is_userland: Optional output pointer; set to 1 if entry point is
 *                   in user-space range (< 0x800000000000), 0 otherwise
 *
 * Validates the ELF header magic, architecture (x86-64), type (executable
 * or shared object), program header table bounds, and PT_LOAD segments.
 * Checks for integer overflows, NULL-page targets, overlapping segments,
 * and alignment requirements. Returns the entry point address if valid.
 *
 * Context: Any context.
 * Return: The ELF entry point address, or 0 if validation fails.
 */
static uint64_t elf_validate(const uint8_t *data, uint64_t size, int *out_is_userland) {
    if (size < sizeof(struct elf64_header)) return 0;

    const struct elf64_header *hdr = (const struct elf64_header *)data;

    /* Validate magic */
    unsigned int elf_magic;
    __builtin_memcpy(&elf_magic, hdr->e_ident, sizeof(elf_magic));
    if (elf_magic != ELF_MAGIC) {
        kprintf("elf: bad magic 0x%08x\n", elf_magic);
        return 0;
    }
    if (hdr->e_ident[4] != ELF_CLASS64) {
        kprintf("elf: not 64-bit\n");
        return 0;
    }
    if (hdr->e_machine != EM_X86_64) {
        kprintf("elf: not x86-64\n");
        return 0;
    }
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        kprintf("elf: not executable\n");
        return 0;
    }
    if (hdr->e_phoff == 0 || hdr->e_phnum == 0) {
        kprintf("elf: no program headers\n");
        return 0;
    }
    if (hdr->e_phnum > ELF_MAX_PHNUM) {
        kprintf("elf: too many program headers (%u > %u)\n",
                hdr->e_phnum, ELF_MAX_PHNUM);
        return 0;
    }
    int userland = (hdr->e_entry < 0x800000000000ULL);
    if (out_is_userland) *out_is_userland = userland;

    /* Validate program header entry size matches expected structure size.
     * If e_phentsize < sizeof(elf64_phdr), individual header reads
     * (p_type, p_offset, p_vaddr, etc.) extend beyond the entry boundary
     * into adjacent data, potentially reading out-of-bounds for the last entry. */
    if (hdr->e_phentsize < sizeof(struct elf64_phdr)) {
        kprintf("elf: program header entry size too small (%u < %zu)\n",
                hdr->e_phentsize, sizeof(struct elf64_phdr));
        return 0;
    }

    /* Validate program header table lies within file, checking for integer
     * overflow.  A crafted e_phoff can wrap the sum so it appears to fit
     * inside 'size', while the actual program header pointer (data + e_phoff)
     * points to arbitrary out-of-bounds memory, yielding a NULL/invalid phdr. */
    uint64_t phdr_end = hdr->e_phoff + (uint64_t)hdr->e_phnum * (uint64_t)hdr->e_phentsize;
    if (phdr_end > size || phdr_end < hdr->e_phoff) {
        kprintf("elf: program header table out of bounds\n");
        return 0;
    }

    /* First pass: collect PT_LOAD segment bounds */
    struct seg_bounds { uint64_t start, end; } segs[64];
    int nsegs = 0;
    uint64_t entry_in_seg = 0;

    for (uint16_t i = 0; i < hdr->e_phnum && nsegs < 64; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(data + hdr->e_phoff + i * hdr->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;
        /* Check for integer overflow before using p_offset + p_filesz */
        if (ph->p_offset > size || ph->p_filesz > size ||
            ph->p_offset + ph->p_filesz > size) {
            kprintf("elf: segment out of bounds (overflow check)\n");
            return 0;
        }
        if (ph->p_filesz > 0 && ph->p_vaddr + ph->p_filesz < ph->p_vaddr) {
            kprintf("elf: segment address overflow\n");
            return 0;
        }
        if (ph->p_memsz > ph->p_filesz &&
            ph->p_vaddr + ph->p_memsz < ph->p_vaddr) {
            kprintf("elf: segment memsz overflow\n");
            return 0;
        }
        if (ph->p_vaddr < 0x1000) {
            kprintf("elf: segment targets NULL page\n");
            return 0;
        }
        if (!userland && ph->p_vaddr < 0x800000000000ULL) {
            kprintf("elf: kernel segment in user-space range\n");
            return 0;
        }

        /* Check p_align is page-aligned for user segments */
        if (ph->p_align > 0 && (ph->p_align & 0xFFF) != 0) {
            kprintf("elf: segment alignment not page-aligned\n");
            return 0;
        }

        /* Check entry point falls in this segment */
        uint64_t seg_end = ph->p_vaddr + ph->p_memsz;
        if (hdr->e_entry >= ph->p_vaddr && hdr->e_entry < seg_end)
            entry_in_seg = 1;

        /* Record bounds for overlap detection */
        segs[nsegs].start = ph->p_vaddr;
        segs[nsegs].end   = seg_end;
        nsegs++;
    }

    /* Entry point must be within at least one PT_LOAD segment */
    if (!entry_in_seg) {
        kprintf("elf: entry point 0x%lx outside all loadable segments\n", (unsigned long)hdr->e_entry);
        return 0;
    }

    /* Check for overlapping segments */
    for (int i = 0; i < nsegs; i++) {
        for (int j = i + 1; j < nsegs; j++) {
            if (segs[i].start < segs[j].end && segs[j].start < segs[i].end) {
                kprintf("elf: overlapping segments [0x%lx-0x%lx] and [0x%lx-0x%lx]\n",
                        (unsigned long)segs[i].start, (unsigned long)segs[i].end,
                        (unsigned long)segs[j].start, (unsigned long)segs[j].end);
                return 0;
            }
        }
    }

    return hdr->e_entry;
}

/**
 * elf_load - Load an ELF binary into memory
 * @data: Pointer to the ELF file data
 * @size: Size of the ELF file data
 *
 * Validates the ELF binary via elf_validate(), then loads PT_LOAD segments
 * into memory. For kernel-mode ELFs (entry >= 0x800000000000), segments
 * are copied directly to their target virtual addresses. For userland ELFs
 * (entry < 0x800000000000), this function only validates and returns the
 * entry point; actual mapping is deferred to the caller.
 *
 * Context: Any context. For kernel ELFs, the target virtual addresses must
 *          already be mapped and writable.
 * Return: The ELF entry point, or 0 on failure.
 */
uint64_t elf_load(const uint8_t *data, uint64_t size) {
    int is_userland = 0;
    uint64_t entry = elf_validate(data, size, &is_userland);
    if (!entry) return 0;

    /* For kernel-mode ELFs, load segments directly (they target mapped vaddrs).
     * For userland ELFs, this is a no-op; the caller does the mapping. */
    if (is_userland) return entry;

    const struct elf64_header *hdr = (const struct elf64_header *)data;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(data + hdr->e_phoff + i * hdr->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;

        /* Copy file bytes to vaddr */
        uint8_t *dst = (uint8_t *)ph->p_vaddr;
        const uint8_t *src = data + ph->p_offset;
        memcpy(dst, src, (size_t)ph->p_filesz);

        /* Zero the BSS (memsz > filesz) */
        if (ph->p_memsz > ph->p_filesz)
            memset(dst + ph->p_filesz, 0, (size_t)(ph->p_memsz - ph->p_filesz));
    }

    return entry;
}

/* Trampoline: each exec'd process gets its own entry-point stub */

/**
 * elf_exec - Execute an ELF binary from a file path
 * @path: Path to the ELF binary file
 *
 * Reads an ELF binary from the filesystem, validates and loads it,
 * creates a new process with appropriate page tables (for user-space
 * ELFs) or a kernel process (for kernel ELFs). For user-space ELFs,
 * it allocates a user stack, maps all PT_LOAD segments with proper
 * permissions, applies ASLR to the stack, and creates a user-mode
 * process running the ELF entry point.
 *
 * Context: May sleep. Allocates memory, reads from VFS, creates processes.
 * Return: 0 on success, or a negative errno (-EINVAL, -ENOMEM, -ENOENT,
 *         -EACCES, -ENOEXEC) on failure.
 */
int elf_exec(const char *path) {
    if (!path) return -EINVAL;

    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (unlikely(!buf)) return -ENOMEM;

    /* Copy path to kernel heap so proc->name is a stable kernel-space pointer,
     * regardless of whether path came from user space or a caller's stack. */
    size_t plen = strlen(path);
    if (plen > 255) plen = 255;
    char *name = (char *)kmalloc(plen + 1);
    if (unlikely(!name)) {
        kfree(buf);
        return -ENOMEM;
    }
    memcpy(name, path, plen);
    name[plen] = '\0';

    uint32_t size = 0;
    if (vfs_read(path, buf, ELF_MAX_SIZE, &size) < 0) {
        kprintf("elf: cannot read %s\n", path);
        kfree(buf);
        kfree(name);
        return -ENOENT;
    }

    /* Check that the ELF header and program header table fit in the read data */
    if ((uint64_t)size < sizeof(struct elf64_header)) {
        kprintf("elf: %s too small for ELF header\n", path);
        kfree(buf);
        kfree(name);
        return -EIO;
    }
    const struct elf64_header *hdr = (const struct elf64_header *)buf;
    uint64_t phdr_end = hdr->e_phoff + (uint64_t)hdr->e_phnum * (uint64_t)hdr->e_phentsize;
    if (phdr_end > (uint64_t)size || phdr_end < hdr->e_phoff) {
        kprintf("elf: %s program header table out of bounds (short read)\n", path);
        kfree(buf);
        kfree(name);
        return -EIO;
    }

    /* Bound program header count before loop */
    if (hdr->e_phnum > ELF_MAX_PHNUM) {
        kprintf("elf: %s too many program headers (%u)\n", path, hdr->e_phnum);
        kfree(buf);
        kfree(name);
        return -EIO;
    }

    /* Check execute permission on the binary file (Item X10) */
    struct process *cur_proc = process_get_current();
    if (process_check_exec_perms(path,
            cur_proc ? cur_proc->euid : 0,
            cur_proc ? cur_proc->egid : 0) < 0) {
        kprintf("elf: permission denied for %s\n", path);
        kfree(buf);
        kfree(name);
        return -EACCES;
    }

    uint64_t entry = elf_load(buf, (unsigned long)size);
    if (unlikely(!entry)) {
        kprintf("elf: load failed\n");
        kfree(buf);
        kfree(name);
        return -ENOEXEC;
    }

    /* Check if this ELF is targeted at user-space addresses (< 0x800000000000) */
    int is_userland = (entry < 0x800000000000ULL);

    if (is_userland) {
        /* Create per-process page tables */
        uint64_t *user_pml4 = vmm_create_user_pml4();
        if (IS_ERR(user_pml4)) {
            kprintf("elf: cannot create page tables\n");
            kfree(buf); kfree(name);
            return -ENOMEM;
        }

        /* Map each PT_LOAD segment into user address space */
        int map_ok = 1;
        for (uint16_t i = 0; i < hdr->e_phnum && map_ok; i++) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(buf + hdr->e_phoff + i * hdr->e_phentsize);
            if (ph->p_type != PT_LOAD) continue;

            if (ph->p_vaddr >= 0x0000800000000000ULL) {
                kprintf("elf: user segment outside user address space\n");
                map_ok = 0; break;
            }

            /* Map pages covering [vaddr, vaddr+memsz) */
            uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
            uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
            uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
            if (ph->p_flags & 2) flags |= VMM_FLAG_WRITE; /* PF_W */
            if (!(ph->p_flags & 1)) flags |= VMM_FLAG_NOEXEC; /* PF_X not set → NX */

            for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
                /* Allocate a physical frame and map it */
                uint64_t frame = pmm_alloc_frame();
                if (unlikely(!frame)) { kprintf("elf: OOM mapping segment\n"); map_ok = 0; break; }
                /* Zero the frame first */
                memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);

                /* Copy segment data to the physical frame via the high-half VMA.
                 * buf is safe because it's a kernel heap address already in the
                 * high-half, reachable regardless of which PML4 is active. */
                uint64_t page_off = va - (ph->p_vaddr & ~0xFFFULL);
                if (ph->p_filesz > page_off) {
                    uint64_t copy_sz = ph->p_filesz - page_off;
                    if (copy_sz > PAGE_SIZE) copy_sz = PAGE_SIZE;
                    memcpy(PHYS_TO_VIRT(frame), buf + ph->p_offset + page_off, copy_sz);
                }

                if (vmm_map_user_page(user_pml4, va, frame, flags) < 0) {
                    kprintf("elf: vmm_map_user_page failed\n");
                    pmm_free_frame(frame);
                    map_ok = 0; break;
                }
            }

            if (!map_ok) break;
        }

        if (!map_ok) {
            vmm_destroy_user_pml4(user_pml4);
            kfree(buf); kfree(name);
            return -ENOMEM;
        }

        /* Allocate user stack (16 pages = 64KB) with guard page at bottom.
         * The guard page (lowest page, at USER_STACK_TOP - USER_STACK_SIZE)
         * is deliberately left unmapped — a stack overflow will hit it and
         * cause a page fault (SIGSEGV).
         * Apply ASLR: shift stack down by a random number of pages. */
        uint64_t aslr_pages_initial = aslr_stack_offset();
        uint64_t user_stack_top = USER_STACK_TOP - (aslr_pages_initial * PAGE_SIZE);
        uint64_t user_stack_bottom = user_stack_top - USER_STACK_SIZE;
        for (uint64_t va = user_stack_bottom + PAGE_SIZE; va < user_stack_top; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (unlikely(!frame)) {
                kprintf("elf: OOM for user stack\n");
                vmm_destroy_user_pml4(user_pml4);
                kfree(buf); kfree(name);
                return -ENOMEM;
            }
            memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
            if (vmm_map_user_page(user_pml4, va, frame,
                                  VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NOEXEC) < 0) {
                kprintf("elf: vmm_map_user_page failed for stack\n");
                pmm_free_frame(frame);
                vmm_destroy_user_pml4(user_pml4);
                kfree(buf); kfree(name);
                return -ENOMEM;
            }
        }

        uint64_t user_rsp = user_stack_top - 8; /* stack grows down */

        (void)hdr;

        kfree(buf);

        /* Create user-mode process */
        struct process *p = process_create_user(entry, user_rsp, user_pml4, name);
        if (unlikely(!p)) {
            kprintf("elf: cannot create user process\n");
            vmm_destroy_user_pml4(user_pml4);
            kfree(name);
            return -ENOMEM;
        }

        p->user_stack_bottom = user_stack_bottom + PAGE_SIZE; /* skip unmapped guard page */
        p->user_stack_top    = user_stack_top;
        kprintf("elf: launched '%s' (pid %lu, ring 3, entry 0x%lx)\n",
                name ? name : "(null)", (unsigned long)p->pid, (unsigned long)entry);
        strncpy(p->exe_path, path, 255);
        p->exe_path[255] = '\0';
        return 0;
    }

    /* Kernel-mode fallback for non-userland ELFs */
    kfree(buf);
    struct process *p = process_create((void (*)(void))(uintptr_t)entry, name);
    if (unlikely(!p)) {
        kprintf("elf: cannot create process\n");
        kfree(name);
        return -ENOMEM;
    }

    kprintf("elf: creating kernel process pid=%lu entry=0x%lx\n",
            (unsigned long)p->pid, (unsigned long)entry);
    strncpy(p->exe_path, path, 255);
    p->exe_path[255] = '\0';
    return 0;
}

/* ── execve: replace current process image with a new ELF ─────────── */

/* These are defined in syscall_asm.asm */
extern volatile uint64_t execve_pending;
extern volatile uint64_t execve_user_rip;
extern volatile uint64_t execve_user_rflags;
extern volatile uint64_t execve_user_rsp;

int process_execve(const char *path, char *const argv[], char *const envp[]) {
    if (!path) return -EINVAL;

    struct process *cur = process_get_current();
    if (!cur) return -EINVAL;
    if (!cur->is_user) return -EACCES;

    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (unlikely(!buf)) return -ENOMEM;

    uint32_t size = 0;
    if (vfs_read(path, buf, ELF_MAX_SIZE, &size) < 0) {
        kfree(buf);
        return -ENOENT;
    }

    /* Check that the ELF header and program header table fit in the read data */
    if ((uint64_t)size < sizeof(struct elf64_header)) {
        kfree(buf);
        return -EIO;
    }
    const struct elf64_header *hdr = (const struct elf64_header *)buf;
    /* Bound program header count before loop */
    if (hdr->e_phnum > ELF_MAX_PHNUM) {
        kfree(buf);
        return -EIO;
    }
    uint64_t phdr_end = hdr->e_phoff + (uint64_t)hdr->e_phnum * (uint64_t)hdr->e_phentsize;
    if (phdr_end > (uint64_t)size || phdr_end < hdr->e_phoff) {
        kfree(buf);
        return -EIO;
    }

    /* Check execute permission on the binary file (Item X10) */
    if (process_check_exec_perms(path, cur->euid, cur->egid) < 0) {
        kfree(buf);
        return -EACCES;
    }

    uint64_t entry = elf_load(buf, (uint64_t)size);
    if (!entry) {
        kfree(buf);
        return -ENOEXEC;
    }

    if (entry >= 0x800000000000ULL) {
        kfree(buf);
        return -ENOEXEC;
    }

    uint64_t *new_pml4 = vmm_create_user_pml4();
    if (IS_ERR(new_pml4)) { kfree(buf); return -ENOMEM; }

    int map_ok = 1;
    for (uint16_t i = 0; i < hdr->e_phnum && map_ok; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(buf + hdr->e_phoff + i * hdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr >= 0x0000800000000000ULL) { map_ok = 0; break; }

        uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (ph->p_flags & 2) flags |= VMM_FLAG_WRITE;
        if (!(ph->p_flags & 1)) flags |= VMM_FLAG_NOEXEC;

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (unlikely(!frame)) { map_ok = 0; break; }
            memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);

            /* Copy segment data via high-half VMA (buf is kernel addr). */
            uint64_t page_off = va - (ph->p_vaddr & ~0xFFFULL);
            if (ph->p_filesz > page_off) {
                uint64_t copy_sz = ph->p_filesz - page_off;
                if (copy_sz > PAGE_SIZE) copy_sz = PAGE_SIZE;
                memcpy(PHYS_TO_VIRT(frame), buf + ph->p_offset + page_off, copy_sz);
            }

            if (vmm_map_user_page(new_pml4, va, frame, flags) < 0) {
                pmm_free_frame(frame);
                map_ok = 0; break;
            }
        }
        if (!map_ok) break;
    }

    kfree(buf); buf = NULL;
    if (!map_ok) { vmm_destroy_user_pml4(new_pml4); return -ENOMEM; }

    /* Close all FD_CLOEXEC file descriptors before exec */
    process_exec_close_cloexec();
    /* Close signalfd fds marked with SFD_CLOEXEC */
    signalfd_exec_close();

    /* ── Check for setuid/setgid on the binary file ──────────────── */
    struct vfs_stat bin_stat;
    int has_setuid = 0;
    if (vfs_stat(path, &bin_stat) == 0) {
        if (bin_stat.mode & S_ISUID) {
            kprintf("execve: setuid (euid %u -> %u)\n", cur->euid, bin_stat.uid);
            cur->euid = bin_stat.uid;
            has_setuid = 1;
        }
        if (bin_stat.mode & S_ISGID) {
            cur->egid = bin_stat.gid;
        }
    }

    /* Apply exec credential security:
     *  - Capability bounding set AND with permitted set
     *  - Securebits processing
     *  - Dumpable flag based on credential changes
     *  - NO_NEW_PRIVS enforcement
     *  - AT_SECURE calculation */
    process_exec_cred_security();

    /* Cancel setuid if no_new_privs was set (NO_NEW_PRIVS blocks elevation) */
    if (cur->no_new_privs && has_setuid) {
        /* process_exec_cred_security already cleared caps;
         * restore euid to the original real uid */
        cur->euid = cur->uid;
        has_setuid = 0;
    }

    /* AT_SECURE flag: set if real != effective uid/gid, or setuid binary */
    int at_secure = (cur->uid != cur->euid || cur->gid != cur->egid || has_setuid);
    (void)at_secure;  /* used later in auxv setup */

    /* Allocate user stack (64KB) with ASLR offset and guard page */
    uint64_t aslr_pages = aslr_stack_offset();
    uint64_t user_stack_top = USER_STACK_TOP - (aslr_pages * PAGE_SIZE);
    uint64_t user_stack_bottom = user_stack_top - USER_STACK_SIZE;
    /* The bottommost page (user_stack_bottom) is deliberately left unmapped
     * as a guard page — a stack underflow past it will fault with SIGSEGV. */
    for (uint64_t va = user_stack_bottom + PAGE_SIZE; va < user_stack_top; va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (unlikely(!frame)) { vmm_destroy_user_pml4(new_pml4); return -ENOMEM; }
        memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
        if (vmm_map_user_page(new_pml4, va, frame,
                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NOEXEC) < 0) {
            pmm_free_frame(frame);
            vmm_destroy_user_pml4(new_pml4);
            return -ENOMEM;
        }
    }
    /* Guard page at user_stack_bottom is left unmapped — a stack underflow
     * (past the bottom) will fault on this page, caught as a SIGSEGV. */
    cur->user_stack_bottom = user_stack_bottom + PAGE_SIZE; /* first mapped stack page */
    cur->user_stack_top    = user_stack_top;

    /* ── Verify no PT_LOAD segment overlaps with the user stack ────── */
    for (uint64_t va = user_stack_bottom; va < user_stack_top; va += PAGE_SIZE) {
        if (vmm_page_is_mapped_user(new_pml4, va)) {
            kprintf("elf: segment overlaps user stack region\n");
            goto fail_cleanup;
        }
    }

    /* ── Set up user stack with argv/envp ───────────────────── */
    /* We're still running on the old page tables. Read argv/envp
     * string pointers from old userspace, copy strings to kernel heap,
     * then lay them out on the new stack via PHYS_TO_VIRT. */

    /* Count argc and envc */
    int argc = 0;
    if (argv) {
        while (argv[argc]) {
            uint64_t ptr = 0;
            memcpy(&ptr, &argv[argc], sizeof(uint64_t));
            (void)ptr;
            argc++;
            if (argc > 256) break;  /* sanity */
        }
    }

    int envc = 0;
    if (envp) {
        while (envp[envc]) {
            uint64_t ptr = 0;
            memcpy(&ptr, &envp[envc], sizeof(uint64_t));
            (void)ptr;
            envc++;
            if (envc > 256) break;
        }
    }

    /* First pass: compute total string size */
    uint64_t total_str_size = 0;
    char *tmp_buf[512];  /* pointers into kernel heap */
    int total_args = argc + envc;

    if (total_args > 0) {
        /* Allocate kernel heap to store copies of all strings */
        for (int i = 0; i < total_args; i++)
            tmp_buf[i] = NULL;

        for (int i = 0; i < argc; i++) {
            const char *s = argv[i];
            if (!s) continue;
            /* Copy string from old userspace, char by char via safe access */
            int len = 0;
            char *kstr = (char *)kmalloc(256);
            if (!kstr) continue;
            while (len < 255) {
                char c;
                memcpy(&c, &s[len], 1);
                if (c == '\0') break;
                kstr[len++] = c;
            }
            kstr[len] = '\0';
            tmp_buf[i] = kstr;
            total_str_size += len + 1;
        }
        for (int i = 0; i < envc; i++) {
            const char *s = (const char *)envp[i];
            if (!s) continue;
            int len = 0;
            char *kstr = (char *)kmalloc(256);
            if (!kstr) continue;
            while (len < 255) {
                char c;
                memcpy(&c, &s[len], 1);
                if (c == '\0') break;
                kstr[len++] = c;
            }
            kstr[len] = '\0';
            tmp_buf[argc + i] = kstr;
            total_str_size += len + 1;
        }
    }

    /* Calculate stack layout from top-down:
     * [strings data] [envp[] + NULL] [argv[] + NULL] [argc]  ← RSP
     *
     * Allocate from the top of the stack downward.
     * We write via PHYS_TO_VIRT of the new stack's physical pages. */
    uint64_t stack_phys_base = 0;
    {
        /* Find the physical address of the user stack bottom */
        int idx4 = (user_stack_bottom >> 39) & 0x1FF;
        int idx3 = (user_stack_bottom >> 30) & 0x1FF;
        int idx2 = (user_stack_bottom >> 21) & 0x1FF;
        int idx1 = (user_stack_bottom >> 12) & 0x1FF;
        if (new_pml4[idx4] & 1) {
            uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(new_pml4[idx4] & ~0xFFFULL);
            if (pdpt[idx3] & 1) {
                uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[idx3] & ~0xFFFULL);
                if (pd[idx2] & 1) {
                    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[idx2] & ~0xFFFULL);
                    stack_phys_base = pt[idx1] & ~0xFFFULL;
                }
            }
        }
    }

    uint64_t stack_top_virt = user_stack_top;
    uint64_t sp = stack_top_virt;  /* start writing from top down */

    /* Check argv+envp total size against user stack capacity.
     * If total_str_size + pointer arrays + argc exceed the stack,
     * return -E2BIG to prevent stack pointer underflow (which would
     * corrupt kernel memory by writing into unmapped/guard region). */
    uint64_t stack_overhead = total_str_size
        + (uint64_t)(argc + 1) * sizeof(uint64_t)   /* argv[] + NULL */
        + (uint64_t)(envc + 1) * sizeof(uint64_t)   /* envp[] + NULL */
        + sizeof(uint64_t);                           /* argc */
    if (stack_overhead > USER_STACK_SIZE - PAGE_SIZE) {
        kprintf("execve: argv+envp too large (%llu bytes > %llu)\n",
                (unsigned long long)stack_overhead,
                (unsigned long long)(USER_STACK_SIZE - PAGE_SIZE));
        /* Clean up tmp_buf allocations before returning */
        for (int i = 0; i < total_args; i++)
            if (tmp_buf[i]) kfree(tmp_buf[i]);
        kfree(buf);
        vmm_destroy_user_pml4(new_pml4);
        return -E2BIG;
    }

    /* Write string data first (at the top of the stack area) */
    sp -= total_str_size;
    sp &= ~0xFULL;  /* align to 16 bytes */

    uint64_t str_pos = sp;
    for (int i = 0; i < argc; i++) {
        if (!tmp_buf[i]) continue;
        char *s = tmp_buf[i];
        int len = (int)strlen(s) + 1;
        /* Copy string to new stack physical page */
        for (int j = 0; j < len; j++) {
            uint64_t va = str_pos + j;
            /* Resolve new stack VA to physical */
            int pi4 = (va >> 39) & 0x1FF;
            int pi3 = (va >> 30) & 0x1FF;
            int pi2 = (va >> 21) & 0x1FF;
            int pi1 = (va >> 12) & 0x1FF;
            if (!(new_pml4[pi4] & 1)) break;
            uint64_t *ppdpt = (uint64_t *)PHYS_TO_VIRT(new_pml4[pi4] & ~0xFFFULL);
            if (!(ppdpt[pi3] & 1)) break;
            uint64_t *ppd = (uint64_t *)PHYS_TO_VIRT(ppdpt[pi3] & ~0xFFFULL);
            if (!(ppd[pi2] & 1)) break;
            uint64_t *ppt = (uint64_t *)PHYS_TO_VIRT(ppd[pi2] & ~0xFFFULL);
            if (!(ppt[pi1] & 1)) break;
            uint64_t phys = (ppt[pi1] & ~0xFFFULL) + (va & 0xFFF);
            *(volatile char *)PHYS_TO_VIRT(phys) = s[j];
        }
        str_pos += len;
    }
    for (int i = 0; i < envc; i++) {
        int idx = argc + i;
        if (!tmp_buf[idx]) continue;
        char *s = tmp_buf[idx];
        int len = (int)strlen(s) + 1;
        for (int j = 0; j < len; j++) {
            uint64_t va = str_pos + j;
            int pi4 = (va >> 39) & 0x1FF;
            int pi3 = (va >> 30) & 0x1FF;
            int pi2 = (va >> 21) & 0x1FF;
            int pi1 = (va >> 12) & 0x1FF;
            if (!(new_pml4[pi4] & 1)) break;
            uint64_t *ppdpt = (uint64_t *)PHYS_TO_VIRT(new_pml4[pi4] & ~0xFFFULL);
            if (!(ppdpt[pi3] & 1)) break;
            uint64_t *ppd = (uint64_t *)PHYS_TO_VIRT(ppdpt[pi3] & ~0xFFFULL);
            if (!(ppd[pi2] & 1)) break;
            uint64_t *ppt = (uint64_t *)PHYS_TO_VIRT(ppd[pi2] & ~0xFFFULL);
            if (!(ppt[pi1] & 1)) break;
            uint64_t phys = (ppt[pi1] & ~0xFFFULL) + (va & 0xFFF);
            *(volatile char *)PHYS_TO_VIRT(phys) = s[j];
        }
        str_pos += len;
    }

    /* Free temporary kernel buffers */
    for (int i = 0; i < total_args; i++) {
        if (tmp_buf[i]) kfree(tmp_buf[i]);
    }

    /* Write envp[] array (envp pointers, then NULL) */
    sp -= (envc + 1) * sizeof(uint64_t);
    sp &= ~0xFULL;
    uint64_t envp_array = sp;
    uint64_t *envp_virt_ptr = (uint64_t *)PHYS_TO_VIRT(stack_phys_base + (envp_array - user_stack_bottom));
    for (int i = 0; i < envc; i++) {
        if (envp_virt_ptr) envp_virt_ptr[i] = envp_array + i * sizeof(uint64_t); /* dummy */
    }
    /* For now, set envp[envc] = NULL */

    /* ── Write auxiliary vector (auxv) entries ───────────────── */
    /* auxv comes after envp[]: envp[] NULL auxv[] AT_NULL argv[] argc */
    sp -= 2 * sizeof(uint64_t);  /* AT_SECURE + value */
    sp &= ~0xFULL;
    {
        uint64_t *aux_secure = (uint64_t *)PHYS_TO_VIRT(
            stack_phys_base + (sp - user_stack_bottom));
        if (aux_secure) {
            aux_secure[0] = AT_SECURE;
            aux_secure[1] = (uint64_t)(uintptr_t)(at_secure ? 1UL : 0UL);
        }
    }

    /* AT_NULL terminator */
    sp -= 2 * sizeof(uint64_t);
    sp &= ~0xFULL;
    {
        uint64_t *aux_null = (uint64_t *)PHYS_TO_VIRT(
            stack_phys_base + (sp - user_stack_bottom));
        if (aux_null) {
            aux_null[0] = AT_NULL;
            aux_null[1] = 0;
        }
    }

    /* Write argv[] array (argv pointers, then NULL) */
    sp -= (argc + 1) * sizeof(uint64_t);
    sp &= ~0xFULL;

    /* Write argc */
    sp -= sizeof(uint64_t);
    uint64_t *argc_ptr = (uint64_t *)PHYS_TO_VIRT(stack_phys_base + (sp - user_stack_bottom));
    if (argc_ptr) *argc_ptr = (uint64_t)argc;

    uint64_t new_rsp = sp;

    /* Update process name and exe path */
    size_t plen = strlen(path);
    if (plen > 255) plen = 255;
    char *kname = (char *)kmalloc(plen + 1);
    if (kname) {
        memcpy(kname, path, plen);
        kname[plen] = '\0';
        cur->name = kname;
    }
    strncpy(cur->exe_path, path, 255);
    cur->exe_path[255] = '\0';

    /* Close all file descriptors with FD_CLOEXEC flag */
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (cur->fd_table[i].used && (cur->fd_table[i].flags & FD_CLOEXEC)) {
            memset(&cur->fd_table[i], 0, sizeof(struct process_fd));
        }
    }

    /* Reset signal handlers per POSIX execve(2) semantics:
     * All caught signals are reset to SIG_DFL. Signals already set to
     * SIG_IGN remain SIG_IGN (preserving the ignore disposition).
     * The signal mask and pending-signal set are preserved. */
    {
        uint64_t __exec_sig_flags;
        spinlock_irqsave_acquire(&cur->sig_lock, &__exec_sig_flags);
        for (int i = 1; i < SIG_MAX; i++) {
            if (cur->sig_handlers[i] != SIG_IGN)
                cur->sig_handlers[i] = SIG_DFL;
        }
        /* Clear per-signal siginfo — stale data from the old process
         * image is meaningless in the new context. Pending signals
         * remain pending and will be delivered with default actions. */
        memset(&cur->sig_info, 0, sizeof(cur->sig_info));
        spinlock_irqsave_release(&cur->sig_lock, __exec_sig_flags);
    }

    /* Destroy old user page tables */
    if (cur->pml4) vmm_destroy_user_pml4(cur->pml4);

    /* Switch to new page tables */
    cur->pml4 = new_pml4;
    cur->user_entry = entry;
    __asm__ volatile("cli");
    vmm_switch_pml4(new_pml4);
    __asm__ volatile("sti");

    /* Set up the execve return state.
     * The syscall return path (syscall_asm.asm) checks execve_pending
     * and uses these values for the sysret instead of the stack state. */
    __asm__ volatile("cli");
    execve_user_rip = entry;
    execve_user_rflags = 0x202;  /* IF=1 */
    execve_user_rsp = new_rsp;
    execve_pending = 1;
    __asm__ volatile("sti");

    kprintf("execve: %s (entry 0x%lx, rsp 0x%lx, pid %lu, argc %d)\n",
            path, (unsigned long)entry, (unsigned long)new_rsp, (unsigned long)cur->pid, argc);
    return 0;

fail_cleanup:
    vmm_destroy_user_pml4(new_pml4);
    kfree(buf);
    return -ENOMEM;
}

/*
 * process_spawn — Lightweight process creation (posix_spawn)
 *
 * Creates a new child process that executes the specified ELF binary.
 * Unlike fork+exec, this does NOT copy the parent's address space —
 * the child starts with a fresh VM layout containing only the ELF
 * segments and a minimal stack.
 *
 * Returns the child PID on success, or a negative errno on failure.
 *
 * Item 306: posix_spawn / posix_spawnp
 */
int process_spawn(const char *path, char *const argv[], char *const envp[])
{
    struct process *parent = process_get_current();
    if (!parent || !parent->is_user)
        return -ECHILD;

    /* ── 0. Check file execute permission ────────────────────────── */
    {
        struct vfs_stat st;
        if (vfs_stat(path, &st) < 0)
            return -ENOENT;
        /* Verify at least one execute bit is set */
        if (!(st.mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            return -EACCES;
        /* If uid matches, owner execute must be set */
        if (st.uid == parent->uid && !(st.mode & S_IXUSR))
            return -EACCES;
        /* If gid matches, group execute must be set */
        if (st.gid == parent->gid && !(st.mode & S_IXGRP))
            return -EACCES;
    }

    /* ── 1. Read the ELF file ───────────────────────────────────── */
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (unlikely(!buf)) return -ENOMEM;

    uint32_t size = 0;
    if (vfs_read(path, buf, ELF_MAX_SIZE, &size) < 0) {
        kfree(buf);
        return -ENOENT;
    }

    /* ── 2. Validate ELF header ─────────────────────────────────── */
    /* Check that the ELF header and program header table fit in the read data */
    if ((uint64_t)size < sizeof(struct elf64_header)) {
        kfree(buf);
        return -EIO;
    }
    const struct elf64_header *hdr = (const struct elf64_header *)buf;
    /* Bound program header count before loop */
    if (hdr->e_phnum > ELF_MAX_PHNUM) {
        kfree(buf);
        return -EIO;
    }
    uint64_t phdr_end = hdr->e_phoff + (uint64_t)hdr->e_phnum * (uint64_t)hdr->e_phentsize;
    if (phdr_end > (uint64_t)size || phdr_end < hdr->e_phoff) {
        kfree(buf);
        return -EIO;
    }

    uint64_t entry = elf_load(buf, (uint64_t)size);
    if (unlikely(!entry)) {
        kfree(buf);
        return -ENOEXEC;
    }

    if (entry >= 0x800000000000ULL) {
        kfree(buf);
        return -ENOEXEC;
    }

    /* ── 3. Create fresh page tables ────────────────────────────── */
    uint64_t *new_pml4 = vmm_create_user_pml4();
    if (IS_ERR(new_pml4)) { kfree(buf); return -ENOMEM; }

    /* ── 4. Map all PT_LOAD segments ────────────────────────────── */
    int map_ok = 1;
    for (uint16_t i = 0; i < hdr->e_phnum && map_ok; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(buf + hdr->e_phoff + i * hdr->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr >= 0x0000800000000000ULL) { map_ok = 0; break; }

        uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (ph->p_flags & 2) flags |= VMM_FLAG_WRITE;
        if (!(ph->p_flags & 1)) flags |= VMM_FLAG_NOEXEC;

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (unlikely(!frame)) { map_ok = 0; break; }
            memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);

            uint64_t page_off = va - (ph->p_vaddr & ~0xFFFULL);
            if (ph->p_filesz > page_off) {
                uint64_t copy_sz = ph->p_filesz - page_off;
                if (copy_sz > PAGE_SIZE) copy_sz = PAGE_SIZE;
                memcpy(PHYS_TO_VIRT(frame), buf + ph->p_offset + page_off, copy_sz);
            }

            if (vmm_map_user_page(new_pml4, va, frame, flags) < 0) {
                pmm_free_frame(frame);
                map_ok = 0; break;
            }
        }
        if (!map_ok) break;
    }

    kfree(buf);
    if (!map_ok) { vmm_destroy_user_pml4(new_pml4); return -ENOMEM; }

    /* ── 5. Allocate user stack (64KB) with ASLR offset ─────────── */
    uint64_t aslr_pages = aslr_stack_offset();
    uint64_t user_stack_top = USER_STACK_TOP - (aslr_pages * PAGE_SIZE);
    uint64_t user_stack_bottom = user_stack_top - USER_STACK_SIZE;
    for (uint64_t va = user_stack_bottom; va < user_stack_top; va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (unlikely(!frame)) { vmm_destroy_user_pml4(new_pml4); return -ENOMEM; }
        memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
        if (vmm_map_user_page(new_pml4, va, frame,
                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NOEXEC) < 0) {
            pmm_free_frame(frame);
            vmm_destroy_user_pml4(new_pml4);
            return -ENOMEM;
        }
    }

    /* ── 6. Count argc and envc ─────────────────────────────────── */
    int argc = 0;
    if (argv) {
        while (argv[argc]) {
            uint64_t ptr = 0;
            memcpy(&ptr, &argv[argc], sizeof(uint64_t));
            (void)ptr;
            argc++;
            if (argc > 256) break;
        }
    }

    int envc = 0;
    if (envp) {
        while (envp[envc]) {
            uint64_t ptr = 0;
            memcpy(&ptr, &envp[envc], sizeof(uint64_t));
            (void)ptr;
            envc++;
            if (envc > 256) break;
        }
    }

    /* ── 7. Copy argv/envp strings to kernel heap ───────────────── */
    uint64_t total_str_size = 0;
    char *tmp_buf[512];
    int total_args = argc + envc;

    if (total_args > 0) {
        for (int i = 0; i < total_args; i++)
            tmp_buf[i] = NULL;

        for (int i = 0; i < argc; i++) {
            const char *s = argv[i];
            if (!s) continue;
            int len = 0;
            char *kstr = (char *)kmalloc(256);
            if (!kstr) continue;
            while (len < 255) {
                char c;
                memcpy(&c, &s[len], 1);
                if (c == '\0') break;
                kstr[len++] = c;
            }
            kstr[len] = '\0';
            tmp_buf[i] = kstr;
            total_str_size += len + 1;
        }
        for (int i = 0; i < envc; i++) {
            const char *s = (const char *)envp[i];
            if (!s) continue;
            int len = 0;
            char *kstr = (char *)kmalloc(256);
            if (!kstr) continue;
            while (len < 255) {
                char c;
                memcpy(&c, &s[len], 1);
                if (c == '\0') break;
                kstr[len++] = c;
            }
            kstr[len] = '\0';
            tmp_buf[argc + i] = kstr;
            total_str_size += len + 1;
        }
    }

    /* ── 8. Lay out stack data (strings, envp[], argv[], argc) ──── */
    uint64_t stack_phys_base = 0;
    {
        int idx4 = (user_stack_bottom >> 39) & 0x1FF;
        int idx3 = (user_stack_bottom >> 30) & 0x1FF;
        int idx2 = (user_stack_bottom >> 21) & 0x1FF;
        int idx1 = (user_stack_bottom >> 12) & 0x1FF;
        if (new_pml4[idx4] & 1) {
            uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(new_pml4[idx4] & ~0xFFFULL);
            if (pdpt[idx3] & 1) {
                uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[idx3] & ~0xFFFULL);
                if (pd[idx2] & 1) {
                    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[idx2] & ~0xFFFULL);
                    stack_phys_base = pt[idx1] & ~0xFFFULL;
                }
            }
        }
    }

    uint64_t sp = user_stack_top;  /* start writing from top down */

    /* Write string data first */
    sp -= total_str_size;
    sp &= ~0xFULL;
    uint64_t str_pos = sp;

    for (int i = 0; i < argc; i++) {
        if (!tmp_buf[i]) continue;
        char *s = tmp_buf[i];
        int len = (int)strlen(s) + 1;
        for (int j = 0; j < len; j++) {
            uint64_t va = str_pos + j;
            int pi4 = (va >> 39) & 0x1FF;
            int pi3 = (va >> 30) & 0x1FF;
            int pi2 = (va >> 21) & 0x1FF;
            int pi1 = (va >> 12) & 0x1FF;
            if (!(new_pml4[pi4] & 1)) break;
            uint64_t *ppdpt = (uint64_t *)PHYS_TO_VIRT(new_pml4[pi4] & ~0xFFFULL);
            if (!(ppdpt[pi3] & 1)) break;
            uint64_t *ppd = (uint64_t *)PHYS_TO_VIRT(ppdpt[pi3] & ~0xFFFULL);
            if (!(ppd[pi2] & 1)) break;
            uint64_t *ppt = (uint64_t *)PHYS_TO_VIRT(ppd[pi2] & ~0xFFFULL);
            if (!(ppt[pi1] & 1)) break;
            uint64_t phys = (ppt[pi1] & ~0xFFFULL) + (va & 0xFFF);
            *(volatile char *)PHYS_TO_VIRT(phys) = s[j];
        }
        str_pos += len;
    }
    for (int i = 0; i < envc; i++) {
        int idx = argc + i;
        if (!tmp_buf[idx]) continue;
        char *s = tmp_buf[idx];
        int len = (int)strlen(s) + 1;
        for (int j = 0; j < len; j++) {
            uint64_t va = str_pos + j;
            int pi4 = (va >> 39) & 0x1FF;
            int pi3 = (va >> 30) & 0x1FF;
            int pi2 = (va >> 21) & 0x1FF;
            int pi1 = (va >> 12) & 0x1FF;
            if (!(new_pml4[pi4] & 1)) break;
            uint64_t *ppdpt = (uint64_t *)PHYS_TO_VIRT(new_pml4[pi4] & ~0xFFFULL);
            if (!(ppdpt[pi3] & 1)) break;
            uint64_t *ppd = (uint64_t *)PHYS_TO_VIRT(ppdpt[pi3] & ~0xFFFULL);
            if (!(ppd[pi2] & 1)) break;
            uint64_t *ppt = (uint64_t *)PHYS_TO_VIRT(ppd[pi2] & ~0xFFFULL);
            if (!(ppt[pi1] & 1)) break;
            uint64_t phys = (ppt[pi1] & ~0xFFFULL) + (va & 0xFFF);
            *(volatile char *)PHYS_TO_VIRT(phys) = s[j];
        }
        str_pos += len;
    }

    /* Free temporary kernel buffers */
    for (int i = 0; i < total_args; i++) {
        if (tmp_buf[i]) kfree(tmp_buf[i]);
    }

    /* Write envp[] array */
    sp -= (envc + 1) * sizeof(uint64_t);
    sp &= ~0xFULL;

    /* Write argv[] array */
    sp -= (argc + 1) * sizeof(uint64_t);
    sp &= ~0xFULL;

    /* Write argc */
    sp -= sizeof(uint64_t);
    uint64_t *argc_ptr = (uint64_t *)PHYS_TO_VIRT(stack_phys_base + (sp - user_stack_bottom));
    if (argc_ptr) *argc_ptr = (uint64_t)argc;

    uint64_t new_rsp = sp;

    /* ── 9. Create the child process ────────────────────────────── */
    struct process *child = process_create_user(entry, new_rsp, new_pml4, path);
    if (unlikely(!child)) {
        vmm_destroy_user_pml4(new_pml4);
        return -ENOMEM;
    }

    /* Inherit parent's credentials, umask, rlimits, and groups */
    child->uid  = parent->uid;
    child->gid  = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->umask = parent->umask;
    child->user_stack_bottom = user_stack_bottom; /* lowest mapped stack page */
    child->user_stack_top    = user_stack_top;
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++)
        child->cap_bset[i] = parent->cap_bset[i];

    /* Inherit parent's file descriptors */
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (parent->fd_table[i].used) {
            child->fd_table[i] = parent->fd_table[i];
        }
    }

    /* Set child name from last component of path */
    const char *slash = path;
    while (*path) { if (*path == '/') slash = path + 1; path++; }
    child->name = slash;

    kprintf("spawn: %s (entry 0x%lx, rsp 0x%lx, child pid %lu)\n",
            child->name, (unsigned long)entry, (unsigned long)new_rsp,
            (unsigned long)child->pid);

    return (int)child->pid;
}

/* ── Stub: elf_load_segment ─────────────────────────────── */
static int elf_load_segment(const void *elf, void *dest, size_t size)
{
    (void)elf;
    (void)dest;
    (void)size;
    kprintf("[elf] elf_load_segment: not yet implemented\n");
    return 0;
}
/* ── Stub: elf_parse_header ─────────────────────────────── */
static int elf_parse_header(const void *data, size_t size)
{
    (void)data;
    (void)size;
    kprintf("[elf] elf_parse_header: not yet implemented\n");
    return 0;
}
/* ── Stub: elf_lookup_sym ─────────────────────────────── */
static void* elf_lookup_sym(const void *elf, const char *name)
{
    (void)elf;
    (void)name;
    kprintf("[elf] elf_lookup_sym: not yet implemented\n");
    return NULL;
}
/* ── Stub: elf_relocate ─────────────────────────────── */
static int elf_relocate(const void *elf, const char *symname, void *addr)
{
    (void)elf;
    (void)symname;
    (void)addr;
    kprintf("[elf] elf_relocate: not yet implemented\n");
    return 0;
}
