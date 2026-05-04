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

uint64_t elf_load(const uint8_t *data, uint64_t size) {
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

    /* Load PT_LOAD segments directly at their vaddr (static, no ASLR) */
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(data + hdr->e_phoff + i * hdr->e_phentsize);

        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_offset + ph->p_filesz > size) {
            kprintf("elf: segment out of bounds\n");
            return 0;
        }

        /* Copy file bytes to vaddr */
        uint8_t *dst = (uint8_t *)ph->p_vaddr;
        const uint8_t *src = data + ph->p_offset;
        memcpy(dst, src, (size_t)ph->p_filesz);

        /* Zero the BSS (memsz > filesz) */
        if (ph->p_memsz > ph->p_filesz)
            memset(dst + ph->p_filesz, 0, (size_t)(ph->p_memsz - ph->p_filesz));
    }

    return hdr->e_entry;
}

/* Trampoline: each exec'd process gets its own entry-point stub */
static uint64_t exec_entry_addr = 0;

static void elf_trampoline(void) {
    /* Jump to the ELF entry point (kernel-mode fallback) */
    void (*entry)(void) = (void (*)(void))exec_entry_addr;
    entry();
    /* If ELF returns, terminate */
    process_exit();
}

int elf_exec(const char *path) {
    uint8_t *buf = (uint8_t *)kmalloc(ELF_MAX_SIZE);
    if (!buf) return -1;

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
            kfree(buf);
            return -1;
        }

        /* Map each PT_LOAD segment into user address space */
        for (uint16_t i = 0; i < hdr->e_phnum; i++) {
            const struct elf64_phdr *ph =
                (const struct elf64_phdr *)(buf + hdr->e_phoff + i * hdr->e_phentsize);
            if (ph->p_type != PT_LOAD) continue;

            /* Map pages covering [vaddr, vaddr+memsz) */
            uint64_t seg_start = ph->p_vaddr & ~0xFFFULL;
            uint64_t seg_end   = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
            uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
            if (ph->p_flags & 2) flags |= VMM_FLAG_WRITE; /* PF_W */

            for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
                /* Allocate a physical frame and map it */
                uint64_t frame = pmm_alloc_frame();
                if (!frame) { kprintf("elf: OOM mapping segment\n"); kfree(buf); return -1; }
                /* Zero the frame first */
                memset((void *)frame, 0, PAGE_SIZE);
                vmm_map_user_page(user_pml4, va, frame, flags);
                /* Also identity-map for now so we can copy data */
                vmm_map_page(va, frame, flags | VMM_FLAG_WRITE);
            }

            /* Copy segment data */
            memcpy((void *)ph->p_vaddr, buf + ph->p_offset, (size_t)ph->p_filesz);
            /* BSS is already zeroed from memset above */
        }

        /* Allocate user stack (16 pages = 64KB) */
        uint64_t user_stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
        for (uint64_t va = user_stack_bottom; va < USER_STACK_TOP; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) { kprintf("elf: OOM for user stack\n"); kfree(buf); return -1; }
            memset((void *)frame, 0, PAGE_SIZE);
            vmm_map_user_page(user_pml4, va, frame,
                              VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
        }

        uint64_t user_rsp = USER_STACK_TOP - 8; /* stack grows down */

        /* Remove the temporary identity maps for code pages */
        /* Not needed — kernel identity maps the whole first 1GB anyway */
        (void)hdr;

        kfree(buf);

        /* Create user-mode process */
        struct process *p = process_create_user(entry, user_rsp, user_pml4, path);
        if (!p) {
            kprintf("elf: cannot create user process\n");
            return -1;
        }

        kprintf("elf: launched %s (pid %u, ring 3, entry 0x%x)\n",
                path, (uint64_t)p->pid, entry);
        return 0;
    }

    /* Kernel-mode fallback for non-userland ELFs */
    kfree(buf);
    exec_entry_addr = entry;
    struct process *p = process_create(elf_trampoline, path);
    if (!p) {
        kprintf("elf: cannot create process\n");
        return -1;
    }

    kprintf("elf: launched %s (pid %u, entry 0x%x)\n",
            path, (uint64_t)p->pid, entry);
    return 0;
}
