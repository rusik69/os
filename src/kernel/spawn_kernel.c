/* src/kernel/spawn_kernel.c — Spawn a userspace process from kernel context.
 *
 * The existing process_spawn() and process_execve() in elf.c check
 * `if (!parent->is_user) return -ECHILD`, making them unusable from
 * kernel context.  This file provides process_spawn_kernel() which
 * creates a user process directly, bypassing the is_user check.
 *
 * It reads the ELF binary, creates per-process page tables, maps the
 * segments and a user stack, and calls process_create_user().
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "elf.h"
#include "process.h"
#include "vmm.h"
#include "pmm.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"

/* Maximum ELF binary size to load */
#define ELF_MAX_SIZE (1024 * 1024)
#define PAGE_SIZE    4096

/* Spawn a userspace process from kernel context (path must be VFS path).
 * Returns PID on success, negative errno on failure. */
int process_spawn_kernel(const char *path) {
    /* Read the ELF binary from VFS */
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) return -ENOMEM;

    uint32_t size = 0;
    if (vfs_read(path, buf, ELF_MAX_SIZE, &size) < 0) {
        kprintf("[spawn_kernel] Cannot read: %s\n", path);
        kfree(buf);
        return -ENOENT;
    }

    /* Validate ELF header */
    const struct elf64_header *hdr = (const struct elf64_header *)buf;
    if (size < sizeof(struct elf64_header) ||
        *(const uint32_t *)hdr->e_ident != ELF_MAGIC ||
        hdr->e_ident[4] != ELF_CLASS64) {
        kprintf("[spawn_kernel] Bad ELF header for: %s\n", path);
        kfree(buf);
        return -ENOEXEC;
    }

    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        kprintf("[spawn_kernel] Not executable: %s\n", path);
        kfree(buf);
        return -ENOEXEC;
    }

    uint64_t entry = hdr->e_entry;
    if (entry >= 0x800000000000ULL) {
        kprintf("[spawn_kernel] Entry in kernel space: %s\n", path);
        kfree(buf);
        return -ENOEXEC;
    }

    /* Create per-process page tables */
    uint64_t *new_pml4 = vmm_create_user_pml4();
    if (!new_pml4) {
        kprintf("[spawn_kernel] Failed to create PML4 for: %s\n", path);
        kfree(buf);
        return -ENOMEM;
    }

    /* Map each PT_LOAD segment */
    int map_ok = 1;
    for (uint16_t i = 0; i < hdr->e_phnum && map_ok; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(buf + hdr->e_phoff +
                                        (uint64_t)i * (uint64_t)hdr->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;

        /* Validate segment is in user space */
        if (ph->p_vaddr >= 0x0000800000000000ULL) {
            kprintf("[spawn_kernel] Segment outside user space\n");
            map_ok = 0; break;
        }

        /* Segment bounds (page-aligned) */
        uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
        uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;

        /* Map flags: user + present by default */
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if (ph->p_flags & 2) flags |= VMM_FLAG_WRITE;   /* PF_W */
        if (!(ph->p_flags & 1)) flags |= VMM_FLAG_NOEXEC; /* PF_X not set */

        for (uint64_t va = seg_start; va < seg_end; va += 0x1000) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) {
                kprintf("[spawn_kernel] OOM mapping segment\n");
                map_ok = 0; break;
            }

            /* Zero the frame first */
            memset(PHYS_TO_VIRT(frame), 0, 0x1000);

            /* Copy segment data to the physical frame */
            uint64_t page_off = va - (ph->p_vaddr & ~0xFFFULL);
            if (ph->p_filesz > page_off) {
                uint64_t copy_sz = ph->p_filesz - page_off;
                if (copy_sz > 0x1000) copy_sz = 0x1000;
                memcpy(PHYS_TO_VIRT(frame),
                       buf + ph->p_offset + page_off, copy_sz);
            }

            if (vmm_map_user_page(new_pml4, va, frame, flags) < 0) {
                kprintf("[spawn_kernel] vmm_map_user_page failed\n");
                pmm_free_frame(frame);
                map_ok = 0; break;
            }
        }
    }

    kfree(buf); /* No longer need the ELF data in kernel heap */

    if (!map_ok) {
        vmm_destroy_user_pml4(new_pml4);
        return -ENOMEM;
    }

    /* Allocate user stack (4 pages = 16 KB) at 0x7FFFF0000000 */
    uint64_t stack_base = 0x7FFFF0000000ULL;
    uint64_t stack_pages = 4;

    for (uint64_t i = 0; i < stack_pages; i++) {
        uint64_t va = stack_base - (stack_pages * 0x1000) + (i * 0x1000);
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            vmm_destroy_user_pml4(new_pml4);
            return -ENOMEM;
        }
        memset(PHYS_TO_VIRT(frame), 0, 0x1000);
        if (vmm_map_user_page(new_pml4, va, frame,
                              VMM_FLAG_PRESENT | VMM_FLAG_USER |
                              VMM_FLAG_WRITE | VMM_FLAG_NOEXEC) < 0) {
            pmm_free_frame(frame);
            vmm_destroy_user_pml4(new_pml4);
            return -ENOMEM;
        }
    }

    /* User RSP: top of stack minus 8 (stack grows down) */
    uint64_t user_rsp = stack_base - 8;

    /* Copy path for process name */
    size_t plen = strlen(path);
    if (plen > 63) plen = 63;
    char *pname = (char *)kmalloc(plen + 1);
    if (pname) {
        memcpy(pname, path, plen);
        pname[plen] = '\0';
    } else {
        pname = (char *)path; /* fallback */
    }

    /* Create the user process */
    struct process *proc = process_create_user(entry, user_rsp, new_pml4, pname);
    if (!proc) {
        kprintf("[spawn_kernel] process_create_user failed for: %s\n", path);
        vmm_destroy_user_pml4(new_pml4);
        if (pname != path) kfree(pname);
        return -ENOMEM;
    }

    /* Set process metadata */
    proc->user_stack_bottom = stack_base - (stack_pages * 0x1000);
    proc->user_stack_top    = stack_base;
    strncpy(proc->exe_path, path, 255);
    proc->exe_path[255] = '\0';

    /* Copy name if we allocated it separately */
    if (pname && pname != path) {
        kfree((void *)proc->name);
        proc->name = pname;
    }

    kprintf("[spawn_kernel] Created PID %d: %s (entry=0x%lx)\n",
            proc->pid, path, (unsigned long)entry);

    return (int)proc->pid;
}

/* ── Stub: spawn_kernel_thread ─────────────────────────────── */
int spawn_kernel_thread(void *fn, void *arg, const char *name)
{
    (void)fn;
    (void)arg;
    (void)name;
    kprintf("[spawn] spawn_kernel_thread: not yet implemented\n");
    return 0;
}
/* ── Stub: spawn_kernel_task ─────────────────────────────── */
int spawn_kernel_task(void *fn, void *arg)
{
    (void)fn;
    (void)arg;
    kprintf("[spawn] spawn_kernel_task: not yet implemented\n");
    return 0;
}
/* ── Stub: spawn_kernel_wait ─────────────────────────────── */
int spawn_kernel_wait(int pid)
{
    (void)pid;
    kprintf("[spawn] spawn_kernel_wait: not yet implemented\n");
    return 0;
}
