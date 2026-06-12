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

/* Max ELF binary we'll try to load from disk */
#define ELF_MAX_SIZE 65536

/* Validate ELF headers and return entry point, WITHOUT copying segments.
 * The caller is responsible for mapping/loading segments afterward. */
static uint64_t elf_validate(const uint8_t *data, uint64_t size, int *out_is_userland) {
    if (size < sizeof(struct elf64_header)) return 0;

    const struct elf64_header *hdr = (const struct elf64_header *)data;

    /* Validate magic */
    if (*(const uint32_t *)hdr->e_ident != ELF_MAGIC) {
        kprintf("elf: bad magic\n");
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

    int userland = (hdr->e_entry < 0x800000000000ULL);
    if (out_is_userland) *out_is_userland = userland;

    /* Validate program header table lies within file */
    if (hdr->e_phoff + (uint64_t)hdr->e_phnum * (uint64_t)hdr->e_phentsize > size) {
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
        if (ph->p_offset + ph->p_filesz > size) {
            kprintf("elf: segment out of bounds\n");
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

int elf_exec(const char *path) {
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) return -1;

    /* Copy path to kernel heap so proc->name is a stable kernel-space pointer,
     * regardless of whether path came from user space or a caller's stack. */
    size_t plen = strlen(path);
    if (plen > 255) plen = 255;
    char *name = (char *)kmalloc(plen + 1);
    if (name) {
        memcpy(name, path, plen);
        name[plen] = '\0';
    } else {
        name = (char *)path; /* fallback: use caller's pointer as-is */
    }

    uint32_t size = 0;
    if (vfs_read(path, buf, ELF_MAX_SIZE, &size) < 0) {
        kprintf("elf: cannot read %s\n", path);
        kfree(buf);
        return -1;
    }
    uint64_t entry = elf_load(buf, (unsigned long)size);
    if (!entry) {
        kprintf("elf: load failed\n");
        kfree(buf);
        return -1;
    }

    /* Check if this ELF is targeted at user-space addresses (< 0x800000000000) */
    const struct elf64_header *hdr = (const struct elf64_header *)buf;
    int is_userland = (entry < 0x800000000000ULL);

    if (is_userland) {
        /* Create per-process page tables */
        uint64_t *user_pml4 = vmm_create_user_pml4();
        if (!user_pml4) {
            kprintf("elf: cannot create page tables\n");
            kfree(buf); kfree(name);
            return -1;
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
                if (!frame) { kprintf("elf: OOM mapping segment\n"); map_ok = 0; break; }
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
            return -1;
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
            if (!frame) {
                kprintf("elf: OOM for user stack\n");
                vmm_destroy_user_pml4(user_pml4);
                kfree(buf); kfree(name);
                return -1;
            }
            memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
            if (vmm_map_user_page(user_pml4, va, frame,
                                  VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NOEXEC) < 0) {
                kprintf("elf: vmm_map_user_page failed for stack\n");
                pmm_free_frame(frame);
                vmm_destroy_user_pml4(user_pml4);
                kfree(buf); kfree(name);
                return -1;
            }
        }

        uint64_t user_rsp = user_stack_top - 8; /* stack grows down */

        (void)hdr;

        kfree(buf);

        /* Create user-mode process */
        struct process *p = process_create_user(entry, user_rsp, user_pml4, name);
        if (!p) {
            kprintf("elf: cannot create user process\n");
            vmm_destroy_user_pml4(user_pml4);
            kfree(name);
            return -1;
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
    if (!p) {
        kprintf("elf: cannot create process\n");
        return -1;
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
    struct process *cur = process_get_current();
    if (!cur) return -1;
    if (!cur->is_user) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) return -1;

    uint32_t size = 0;
    if (vfs_read(path, buf, ELF_MAX_SIZE, &size) < 0) {
        kfree(buf);
        return -1;
    }

    uint64_t entry = elf_load(buf, (uint64_t)size);
    if (!entry) {
        kfree(buf);
        return -1;
    }

    const struct elf64_header *hdr = (const struct elf64_header *)buf;
    if (entry >= 0x800000000000ULL) {
        kfree(buf);
        return -1;
    }

    uint64_t *new_pml4 = vmm_create_user_pml4();
    if (!new_pml4) { kfree(buf); return -1; }

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
            if (!frame) { map_ok = 0; break; }
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

    kfree(buf);
    if (!map_ok) { vmm_destroy_user_pml4(new_pml4); return -1; }

    /* Close all FD_CLOEXEC file descriptors before exec */
    process_exec_close_cloexec();
    /* Close signalfd fds marked with SFD_CLOEXEC */
    extern void signalfd_exec_close(void);
    signalfd_exec_close();

    /* Apply exec credential security:
     *  - Capability bounding set AND with permitted set
     *  - Securebits processing
     *  - Dumpable flag based on credential changes
     *  - NO_NEW_PRIVS enforcement */
    process_exec_cred_security();

    /* Allocate user stack (64KB) with ASLR offset and guard page */
    uint64_t aslr_pages = aslr_stack_offset();
    uint64_t user_stack_top = USER_STACK_TOP - (aslr_pages * PAGE_SIZE);
    uint64_t user_stack_bottom = user_stack_top - USER_STACK_SIZE;
    uint64_t user_stack_guard = user_stack_bottom - PAGE_SIZE;  /* unmapped guard page */
    for (uint64_t va = user_stack_bottom; va < user_stack_top; va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) { vmm_destroy_user_pml4(new_pml4); return -1; }
        memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);
        if (vmm_map_user_page(new_pml4, va, frame,
                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER | VMM_FLAG_NOEXEC) < 0) {
            pmm_free_frame(frame);
            vmm_destroy_user_pml4(new_pml4);
            return -1;
        }
    }
    /* Guard page at user_stack_guard is left unmapped — a stack underflow
     * (past the bottom) will fault on this page, caught as a SIGSEGV. */
    cur->user_stack_bottom = user_stack_bottom; /* lowest mapped stack page */
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

    /* Write string data first (at the top of the stack area) */
    sp -= total_str_size;
    sp &= ~0xFULL;  /* align to 16 bytes */

    uint64_t str_pos = sp;
    for (int i = 0; i < argc; i++) {
        if (!tmp_buf[i]) continue;
        char *s = tmp_buf[i];
        int len = strlen(s) + 1;
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
        int len = strlen(s) + 1;
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
        /* Compute address of string data */
        uint64_t str_addr = str_pos;
        for (int k = 0; k < i; k++) {
            int k_idx = argc + k;
            if (tmp_buf[k_idx])
                str_addr -= strlen(tmp_buf[k_idx]) + 1;
        }
        if (envp_virt_ptr) envp_virt_ptr[i] = envp_array + i * sizeof(uint64_t); /* dummy */
    }
    /* For now, set envp[envc] = NULL */

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
    return -1;
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

    /* ── 1. Read the ELF file ───────────────────────────────────── */
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) return -ENOMEM;

    uint32_t size = 0;
    if (vfs_read(path, buf, ELF_MAX_SIZE, &size) < 0) {
        kfree(buf);
        return -ENOENT;
    }

    /* ── 2. Validate ELF header ─────────────────────────────────── */
    uint64_t entry = elf_load(buf, (uint64_t)size);
    if (!entry) {
        kfree(buf);
        return -ENOEXEC;
    }

    const struct elf64_header *hdr = (const struct elf64_header *)buf;
    if (entry >= 0x800000000000ULL) {
        kfree(buf);
        return -ENOEXEC;
    }

    /* ── 3. Create fresh page tables ────────────────────────────── */
    uint64_t *new_pml4 = vmm_create_user_pml4();
    if (!new_pml4) { kfree(buf); return -ENOMEM; }

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
            if (!frame) { map_ok = 0; break; }
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
        if (!frame) { vmm_destroy_user_pml4(new_pml4); return -ENOMEM; }
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
        int len = strlen(s) + 1;
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
        int len = strlen(s) + 1;
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
    if (!child) {
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
