#include "elf.h"
#include "vfs.h"
#include "process.h"
#include "heap.h"
#include "string.h"
#include "printf.h"

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
    /* Jump to the ELF entry point */
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
    kfree(buf);

    if (!entry) {
        kprintf("elf: load failed\n");
        return -1;
    }

    /* Store entry point so the trampoline can find it */
    exec_entry_addr = entry;

    /* Create a new kernel process whose code starts at the ELF entry */
    struct process *p = process_create(elf_trampoline, path);
    if (!p) {
        kprintf("elf: cannot create process\n");
        return -1;
    }

    kprintf("elf: launched %s (pid %u, entry 0x%x)\n",
            path, (uint64_t)p->pid, entry);
    return 0;
}
