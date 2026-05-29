#include "elf.h"
#include "vfs.h"
#include "process.h"
#include "heap.h"
#include "string.h"
#include "printf.h"
#include "vmm.h"
#include "pmm.h"

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

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
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
        if (!userland && ph->p_vaddr >= 0x800000000000ULL) {
            kprintf("elf: kernel segment in user-space range\n");
            return 0;
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
    uint64_t entry = elf_load(buf, (uint64_t)size);
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

            for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
                /* Allocate a physical frame and map it */
                uint64_t frame = pmm_alloc_frame();
                if (!frame) { kprintf("elf: OOM mapping segment\n"); map_ok = 0; break; }
                /* Zero the frame first */
                memset((void *)frame, 0, PAGE_SIZE);

                /* Copy segment data directly to the physical frame via identity map.
                 * Can't switch to user PML4 for this because buf is a kernel address
                 * not reachable from user page tables. */
                uint64_t page_off = va - (ph->p_vaddr & ~0xFFFULL);
                if (ph->p_filesz > page_off) {
                    uint64_t copy_sz = ph->p_filesz - page_off;
                    if (copy_sz > PAGE_SIZE) copy_sz = PAGE_SIZE;
                    memcpy((void *)frame, buf + ph->p_offset + page_off, copy_sz);
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

        /* Allocate user stack (16 pages = 64KB) */
        uint64_t user_stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
        for (uint64_t va = user_stack_bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) {
                kprintf("elf: OOM for user stack\n");
                vmm_destroy_user_pml4(user_pml4);
                kfree(buf); kfree(name);
                return -1;
            }
            memset((void *)frame, 0, PAGE_SIZE);
            if (vmm_map_user_page(user_pml4, va, frame,
                                  VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER) < 0) {
                kprintf("elf: vmm_map_user_page failed for stack\n");
                pmm_free_frame(frame);
                vmm_destroy_user_pml4(user_pml4);
                kfree(buf); kfree(name);
                return -1;
            }
        }

        uint64_t user_rsp = USER_STACK_TOP - 8; /* stack grows down */

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

        kprintf("elf: launched %s (pid %u, ring 3, entry 0x%x)\n",
                name, (uint64_t)p->pid, entry);
        return 0;
    }

    /* Kernel-mode fallback for non-userland ELFs */
    kfree(buf);
    struct process *p = process_create((void (*)(void))(uintptr_t)entry, name);
    if (!p) {
        kprintf("elf: cannot create process\n");
        return -1;
    }

    kprintf("elf: launched %s (pid %u, entry 0x%x)\n",
            name, (uint64_t)p->pid, entry);
    return 0;
}

/* ── execve: replace current process image with a new ELF ─────────── */

/* These are defined in syscall_asm.asm */
extern volatile uint64_t execve_pending;
extern volatile uint64_t execve_user_rip;
extern volatile uint64_t execve_user_rflags;
extern volatile uint64_t execve_user_rsp;

int process_execve(const char *path, char *const argv[], char *const envp[]) {
    (void)argv; (void)envp;

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

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) { map_ok = 0; break; }
            memset((void *)frame, 0, PAGE_SIZE);

            /* Copy segment data via identity map (buf is kernel addr). */
            uint64_t page_off = va - (ph->p_vaddr & ~0xFFFULL);
            if (ph->p_filesz > page_off) {
                uint64_t copy_sz = ph->p_filesz - page_off;
                if (copy_sz > PAGE_SIZE) copy_sz = PAGE_SIZE;
                memcpy((void *)frame, buf + ph->p_offset + page_off, copy_sz);
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

    /* Allocate user stack (64KB) */
    uint64_t user_stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
    for (uint64_t va = user_stack_bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) { vmm_destroy_user_pml4(new_pml4); return -1; }
        memset((void *)frame, 0, PAGE_SIZE);
        if (vmm_map_user_page(new_pml4, va, frame,
                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER) < 0) {
            pmm_free_frame(frame);
            vmm_destroy_user_pml4(new_pml4);
            return -1;
        }
    }

    uint64_t new_rsp = USER_STACK_TOP - 8;

    /* Update process name */
    size_t plen = strlen(path);
    if (plen > 255) plen = 255;
    char *kname = (char *)kmalloc(plen + 1);
    if (kname) {
        memcpy(kname, path, plen);
        kname[plen] = '\0';
        cur->name = kname;
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

    kprintf("execve: %s (entry 0x%x, rsp 0x%x, pid %u)\n",
            path, entry, new_rsp, (uint64_t)cur->pid);
    return 0;
}
