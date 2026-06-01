#define KERNEL_INTERNAL
#include "coredump.h"
#include "process.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "elf.h"
#include "vmm.h"

/* Core dump helper — writes a minimal ELF core dump for a process */
static char coredump_path[COREDUMP_PATH_MAX] = "/core";

void coredump_init(void) {
    kprintf("[OK] coredump helper initialized (path=%s)\n", coredump_path);
}

void coredump_set_path(const char *path) {
    if (path) {
        strncpy(coredump_path, path, COREDUMP_PATH_MAX - 1);
        coredump_path[COREDUMP_PATH_MAX - 1] = '\0';
    }
}

const char *coredump_get_path(void) {
    return coredump_path;
}

int coredump_enabled(uint32_t pid) {
    struct process *p = process_get_by_pid(pid);
    if (!p) return 0;
    return p->coredump_enabled;
}

/* Write an ELF core dump for a process */
int coredump_write(uint32_t pid, const char *path) {
    struct process *p = process_get_by_pid(pid);
    if (!p) return -ESRCH;

    char dump_path[128];
    if (path) {
        strncpy(dump_path, path, sizeof(dump_path) - 1);
        dump_path[sizeof(dump_path) - 1] = '\0';
    } else {
        snprintf(dump_path, sizeof(dump_path), "%s.%u", coredump_path, pid);
    }

    /* We write a minimal ELF note: just PRSTATUS + NT_PRSTATUS note */
    /* For a hobby kernel, we write a simplified ELF dump */

    /* ELF header for core file */
    struct {
        uint8_t  e_ident[16];
        uint16_t e_type;
        uint16_t e_machine;
        uint32_t e_version;
        uint64_t e_entry;
        uint64_t e_phoff;
        uint64_t e_shoff;
        uint32_t e_flags;
        uint16_t e_ehsize;
        uint16_t e_phentsize;
        uint16_t e_phnum;
        uint16_t e_shentsize;
        uint16_t e_shnum;
        uint16_t e_shstrndx;
    } __attribute__((packed)) elf_hdr;

    memset(&elf_hdr, 0, sizeof(elf_hdr));
    elf_hdr.e_ident[0] = 0x7f;
    elf_hdr.e_ident[1] = 'E';
    elf_hdr.e_ident[2] = 'L';
    elf_hdr.e_ident[3] = 'F';
    elf_hdr.e_ident[4] = 2;      /* ELF64 */
    elf_hdr.e_ident[5] = 2;      /* big-endian? no, little-endian=1 */
    elf_hdr.e_ident[5] = 1;      /* little-endian */
    elf_hdr.e_ident[6] = 1;      /* ELF version */
    elf_hdr.e_type = 4;           /* ET_CORE */
    elf_hdr.e_machine = 0x3E;    /* x86-64 */
    elf_hdr.e_version = 1;
    elf_hdr.e_phoff = sizeof(elf_hdr);
    elf_hdr.e_ehsize = sizeof(elf_hdr);
    elf_hdr.e_phentsize = sizeof(struct {
        uint32_t p_type;
        uint32_t p_flags;
        uint64_t p_offset;
        uint64_t p_vaddr;
        uint64_t p_paddr;
        uint64_t p_filesz;
        uint64_t p_memsz;
        uint64_t p_align;
    } __attribute__((packed)));
    elf_hdr.e_phnum = 2; /* one NOTE, one LOAD */

    /* Program headers */
    uint8_t phdr_buf[sizeof(uint32_t)*2 + sizeof(uint64_t)*6]; /* 56 bytes * 2 */
    memset(phdr_buf, 0, sizeof(phdr_buf));

    /* Write to file via VFS */
    int ret = vfs_create(dump_path, 1); /* 1 = file type */
    if (ret < 0 && ret != -EEXIST) {
        kprintf("[coredump] failed to create %s: err=%d\n", dump_path, ret);
        return ret;
    }

    /* Write ELF header */
    ret = vfs_write(dump_path, &elf_hdr, sizeof(elf_hdr));
    if (ret < 0) {
        kprintf("[coredump] failed to write ELF header: err=%d\n", ret);
        return ret;
    }

    /* Write program headers (stub — simplified) */
    uint32_t phdr_type_note = 4;  /* PT_NOTE */
    /* Write two phdrs: NOTE and LOAD */
    uint8_t note_phdr[56];
    memset(note_phdr, 0, 56);
    *(uint32_t*)(note_phdr + 0) = 4;  /* PT_NOTE */
    *(uint32_t*)(note_phdr + 4) = 0;  /* flags */
    *(uint64_t*)(note_phdr + 8) = sizeof(elf_hdr) + 56*2; /* offset */
    *(uint64_t*)(note_phdr + 16) = 0; /* vaddr */
    *(uint64_t*)(note_phdr + 24) = 0; /* paddr */
    *(uint64_t*)(note_phdr + 32) = sizeof(struct { uint32_t namesz; uint32_t descsz; uint32_t type; char name[8]; }) +
                                   sizeof(struct { uint32_t si_signo; uint32_t si_code; uint32_t si_errno; uint32_t pr_pid; });
    *(uint64_t*)(note_phdr + 40) = *(uint64_t*)(note_phdr + 32); /* memsz = filesz */
    *(uint64_t*)(note_phdr + 48) = 0; /* align */

    uint8_t load_phdr[56];
    memset(load_phdr, 0, 56);
    *(uint32_t*)(load_phdr + 0) = 1;  /* PT_LOAD */
    *(uint32_t*)(load_phdr + 4) = 7;  /* flags = RWX */
    *(uint64_t*)(load_phdr + 8) = sizeof(elf_hdr) + 56*2 + *(uint64_t*)(note_phdr + 32); /* offset after notes */
    *(uint64_t*)(load_phdr + 16) = 0; /* vaddr */
    *(uint64_t*)(load_phdr + 24) = 0; /* paddr */
    *(uint64_t*)(load_phdr + 32) = 0; /* filesz */
    *(uint64_t*)(load_phdr + 40) = KERNEL_STACK_SIZE + PAGE_SIZE; /* memsz */
    *(uint64_t*)(load_phdr + 48) = PAGE_SIZE; /* align */

    ret = vfs_write(dump_path, note_phdr, sizeof(note_phdr));
    if (ret < 0) return ret;
    ret = vfs_write(dump_path, load_phdr, sizeof(load_phdr));
    if (ret < 0) return ret;

    /* Write NT_PRSTATUS note */
    struct {
        uint32_t namesz;
        uint32_t descsz;
        uint32_t type;
        char name[8];
    } __attribute__((packed)) note_hdr;
    memset(&note_hdr, 0, sizeof(note_hdr));
    note_hdr.namesz = 8;  /* includes NUL */
    note_hdr.type = 1;    /* NT_PRSTATUS */
    memcpy(note_hdr.name, "CORE", 5);

    struct {
        uint32_t si_signo;
        uint32_t si_code;
        uint32_t si_errno;
        uint32_t pr_pid;
    } __attribute__((packed)) prstatus;
    memset(&prstatus, 0, sizeof(prstatus));
    prstatus.pr_pid = pid;
    note_hdr.descsz = sizeof(prstatus);

    ret = vfs_write(dump_path, &note_hdr, sizeof(note_hdr));
    if (ret < 0) return ret;
    ret = vfs_write(dump_path, &prstatus, sizeof(prstatus));
    if (ret < 0) return ret;

    kprintf("[coredump] wrote core dump %s for pid=%u\n", dump_path, pid);
    return 0;
}
