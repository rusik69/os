/*
 * dlfcn.c — dlopen/dlsym/dlclose/dlerror implementation (Item U19)
 *
 * Loads ELF ET_DYN shared objects (.so) into the process address space.
 * This is a userspace-focused implementation using syscall wrappers
 * (SYS_OPEN, SYS_READ, SYS_MMAP, etc.) to load and relocate shared
 * libraries.
 *
 * Limitation: This does NOT implement full dynamic linking with lazy
 * symbol resolution via PLT/GOT.  RTLD_NOW and RTLD_LAZY both resolve
 * all R_X86_64_RELATIVE relocations immediately;  GOT/PLT entries are
 * resolved on first dlsym() or at load time via the DT_JMPREL table.
 *
 * Thread safety: Uses a simple global lock (via kernel mutex)
 * to protect the loaded-library list.
 */

#include "dlfcn.h"
#include "elf.h"
#include "syscall.h"
#include "string.h"
#include "types.h"
#include "printf.h"
#include "heap.h"

/* ── Internal constants ─────────────────────────────────────────────── */

/* Load area for shared libraries: start just above the stack guard area */
#define DL_DEFAULT_BASE    0x00007F0000000000ULL
#define DL_REGION_SIZE     0x0000010000000000ULL /* 64 GB virtual space */

/* Max number of concurrently loaded shared objects */
#define DL_MAX_HANDLES     64

/* Magic constant for handle validation */
#define DL_MAGIC           0x444C4F4144000001ULL /* "DLOAD\x00\x00\x01" */

/* Segment/allocation limits per loaded object */
#define DL_OBJ_MAX_SEGMENTS 8
#define DL_OBJ_MAX_DEPS    8
#define DL_OBJ_MAX_NAME    128

/* Default library search paths (colon-separated) */
#define DL_DEFAULT_LIB_PATH "/lib:/usr/lib:/usr/local/lib"

/* Page size */
#define DL_PAGE_SIZE       4096UL
#define DL_PAGE_MASK       (DL_PAGE_SIZE - 1)

/* ── Forward declarations ──────────────────────────────────────────── */

struct dl_loaded_obj;

/* ── External kernel functions for syscall and memory ──────────────── */

/* libc_syscall from syscall_asm.asm */
extern uint64_t libc_syscall(uint64_t num, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5);

/* Heap allocator (defined in src/memory/heap.c) */
extern void *kmalloc(size_t size);
extern void  kfree(void *ptr);

/* Mutex (kernel mutex, defined in src/ipc/mutex.c) */
extern int  mutex_init(void);
extern void mutex_lock(int id);
extern void mutex_unlock(int id);

/* ── Internal syscall wrappers ─────────────────────────────────────── */

static inline int dl_open(const char *path, int flags) {
    return (int)(int64_t)libc_syscall(SYS_OPEN,
        (uint64_t)(uintptr_t)path, (uint64_t)flags, 0, 0, 0);
}

static inline int dl_read(int fd, void *buf, uint32_t count) {
    return (int)(int64_t)libc_syscall(SYS_READ,
        (uint64_t)fd, (uint64_t)(uintptr_t)buf, (uint64_t)count, 0, 0);
}

static inline int dl_close(int fd) {
    return (int)(int64_t)libc_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
}

static inline int64_t dl_lseek(int fd, int64_t offset, int whence) {
    return (int64_t)libc_syscall(SYS_LSEEK,
        (uint64_t)fd, (uint64_t)offset, (uint64_t)whence, 0, 0);
}

static inline void *dl_mmap(uint64_t addr, uint64_t length, uint64_t prot) {
    return (void *)libc_syscall(SYS_MMAP, addr, length, prot, 0, 0);
}

static inline int dl_munmap(uint64_t addr, uint64_t length) {
    return (int)(int64_t)libc_syscall(SYS_MUNMAP, addr, length, 0, 0, 0);
}

/* ── Segment descriptor for a loaded shared object ─────────────────── */

struct dl_segment {
    uint64_t vaddr;       /* virtual address of segment (base + p_vaddr) */
    uint64_t memsz;       /* segment size in memory (p_memsz) */
    uint64_t filesz;      /* segment size in file (p_filesz) */
    uint32_t prot;        /* protection flags (PROT_READ/WRITE/EXEC) */
};

/* ── Loaded shared object descriptor ───────────────────────────────── */

struct dl_loaded_obj {
    uint64_t       magic;              /* DL_MAGIC for validation */
    char           name[DL_OBJ_MAX_NAME]; /* library name / soname */
    void          *handle;             /* opaque handle == &this struct */

    uint64_t       base_addr;          /* load base address */

    int            seg_count;
    struct dl_segment segments[DL_OBJ_MAX_SEGMENTS];

    /* Dynamic section info */
    uint64_t       dyn_symtab;         /* address of .dynsym (absolute) */
    uint64_t       dyn_strtab;         /* address of .dynstr (absolute) */
    uint64_t       dyn_strsz;          /* size of .dynstr */
    uint64_t       dyn_syment;         /* size of each symbol entry */
    int            dyn_sym_count;      /* number of symbols (approx) */

    uint64_t       init_func;          /* address of DT_INIT (or 0) */
    uint64_t       fini_func;          /* address of DT_FINI (or 0) */
    uint64_t      *init_array;         /* address of DT_INIT_ARRAY */
    uint64_t       init_array_size;    /* size of init array */
    uint64_t      *fini_array;         /* address of DT_FINI_ARRAY */
    uint64_t       fini_array_size;    /* size of fini array */

    uint64_t       rela_dyn;           /* address of DT_RELA (absolute) */
    uint64_t       rela_dyn_size;      /* DT_RELASZ */
    uint64_t       rela_plt;           /* address of DT_JMPREL (absolute) */
    uint64_t       rela_plt_size;      /* DT_PLTRELSZ */
    uint64_t       rela_plt_ent;       /* DT_PLTREL (0=REL, 1=RELA) */

    uint64_t       pltgot;             /* address of DT_PLTGOT */

    /* Reference counting and flags */
    int            refcount;
    int            flags;              /* RTLD_GLOBAL, etc. */
    int            loaded;             /* 1 = fully loaded + init'd */

    /* Dependencies */
    int            dep_count;
    struct dl_loaded_obj *deps[DL_OBJ_MAX_DEPS];
};

/* ── Global state ──────────────────────────────────────────────────── */

static struct dl_loaded_obj *loaded_handles[DL_MAX_HANDLES];
static int handle_count = 0;
static int dl_mutex_id = -1;

/* Error string buffer (static) */
static char dl_errbuf[256];
static int  dl_err_set = 0;

/* Next base address for library loading */
static uint64_t dl_next_base = DL_DEFAULT_BASE;

/* ── Error handling ────────────────────────────────────────────────── */

static void dl_set_error(const char *msg) {
    int i;
    for (i = 0; i < 255 && msg[i] != '\0'; i++)
        dl_errbuf[i] = msg[i];
    dl_errbuf[i] = '\0';
    dl_err_set = 1;
}

static void dl_clear_error(void) {
    dl_errbuf[0] = '\0';
    dl_err_set = 0;
}

char *dlerror(void) {
    if (!dl_err_set) return NULL;
    dl_err_set = 0;
    return dl_errbuf;
}

/* ── Locking ────────────────────────────────────────────────────────── */

static void dl_lock(void) {
    if (dl_mutex_id < 0) {
        dl_mutex_id = mutex_init();
    }
    if (dl_mutex_id >= 0) {
        mutex_lock(dl_mutex_id);
    }
}

static void dl_unlock(void) {
    if (dl_mutex_id >= 0) {
        mutex_unlock(dl_mutex_id);
    }
}

/* ── Library list management ───────────────────────────────────────── */

/* Find a loaded library by name (exact match). */
static struct dl_loaded_obj *dl_find_loaded(const char *name) {
    for (int i = 0; i < handle_count; i++) {
        if (loaded_handles[i] &&
            strcmp(loaded_handles[i]->name, name) == 0) {
            return loaded_handles[i];
        }
    }
    return NULL;
}

/* Add a loaded object to the global list. */
static int dl_register(struct dl_loaded_obj *obj) {
    if (handle_count >= DL_MAX_HANDLES)
        return -1;
    loaded_handles[handle_count++] = obj;
    return 0;
}

/* Remove from list by handle. */
static int dl_unregister(void *handle) {
    for (int i = 0; i < handle_count; i++) {
        if (loaded_handles[i] == (struct dl_loaded_obj *)handle) {
            for (int j = i; j < handle_count - 1; j++)
                loaded_handles[j] = loaded_handles[j + 1];
            loaded_handles[handle_count - 1] = NULL;
            handle_count--;
            return 0;
        }
    }
    return -1;
}

/* ── ELF loading helpers ────────────────────────────────────────────── */

/* Read the entire .so file into a malloc'd buffer. */
static uint8_t *dl_read_whole_file(const char *path, uint64_t *out_size) {
    int fd = dl_open(path, 0);
    if (fd < 0) {
        dl_set_error("dlopen: cannot open file");
        return NULL;
    }

    int64_t filesize = dl_lseek(fd, 0, 2); /* SEEK_END */
    if (filesize <= 0) {
        dl_close(fd);
        dl_set_error("dlopen: empty or unseekable file");
        return NULL;
    }

    dl_lseek(fd, 0, 0);

    uint8_t *buf = (uint8_t *)kmalloc((size_t)filesize + 1);
    if (!buf) {
        dl_close(fd);
        dl_set_error("dlopen: out of memory reading file");
        return NULL;
    }

    int total_read = 0;
    while (total_read < filesize) {
        int n = dl_read(fd, buf + total_read,
                        (uint32_t)(filesize - total_read));
        if (n <= 0) {
            kfree(buf);
            dl_close(fd);
            dl_set_error("dlopen: read error");
            return NULL;
        }
        total_read += n;
    }

    dl_close(fd);
    buf[filesize] = 0;
    *out_size = (uint64_t)filesize;
    return buf;
}

/* Validate ELF header: must be ET_DYN, x86-64. */
static int dl_validate_elf(const uint8_t *data, uint64_t size) {
    if (size < sizeof(struct elf64_header)) {
        dl_set_error("dlopen: file too small for ELF header");
        return -1;
    }

    const struct elf64_header *ehdr = (const struct elf64_header *)data;

    if (*(const uint32_t *)ehdr->e_ident != ELF_MAGIC) {
        dl_set_error("dlopen: not an ELF file");
        return -1;
    }
    if (ehdr->e_ident[4] != ELF_CLASS64) {
        dl_set_error("dlopen: not a 64-bit ELF file");
        return -1;
    }
    if (ehdr->e_ident[5] != ELF_DATA2LSB) {
        dl_set_error("dlopen: not little-endian ELF");
        return -1;
    }
    if (ehdr->e_machine != EM_X86_64) {
        dl_set_error("dlopen: not an x86-64 ELF");
        return -1;
    }
    if (ehdr->e_type != ET_DYN) {
        dl_set_error("dlopen: not a shared object");
        return -1;
    }
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        dl_set_error("dlopen: no program headers");
        return -1;
    }
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum *
        sizeof(struct elf64_phdr) > size) {
        dl_set_error("dlopen: program headers extend past file");
        return -1;
    }

    return 0;
}

/* ── PT_LOAD segment loading ────────────────────────────────────────── */

static uint32_t dl_pflags_to_mmap_prot(uint32_t p_flags) {
    uint32_t prot = 0;
    if (p_flags & 0x4) prot |= 1; /* PROT_READ */
    if (p_flags & 0x2) prot |= 2; /* PROT_WRITE */
    if (p_flags & 0x1) prot |= 4; /* PROT_EXEC */
    return prot;
}

/* Map a single PT_LOAD segment at base + p_vaddr. */
static int dl_map_segment(struct dl_loaded_obj *obj,
                           const uint8_t *file_data,
                           const struct elf64_phdr *phdr) {
    if (obj->seg_count >= DL_OBJ_MAX_SEGMENTS)
        return -1;

    uint64_t vaddr = obj->base_addr + phdr->p_vaddr;
    uint64_t filesz = phdr->p_filesz;
    uint64_t memsz = phdr->p_memsz;
    uint32_t prot = dl_pflags_to_mmap_prot(phdr->p_flags);

    uint64_t page_off = vaddr & DL_PAGE_MASK;
    uint64_t map_addr = vaddr & ~DL_PAGE_MASK;
    uint64_t map_size = memsz + page_off;
    map_size = (map_size + DL_PAGE_SIZE - 1) & ~DL_PAGE_MASK;

    void *result = dl_mmap(map_addr, map_size, prot);
    if (result == (void *)-1 || result == NULL) {
        dl_set_error("dlopen: mmap failed for segment");
        return -1;
    }

    /* Copy segment data from file buffer */
    if (filesz > 0) {
        const uint8_t *src = file_data + phdr->p_offset;
        uint8_t *dst = (uint8_t *)vaddr;
        uint64_t copy_size = filesz < memsz ? filesz : memsz;
        for (uint64_t i = 0; i < copy_size; i++)
            dst[i] = src[i];
    }

    /* Zero-fill BSS (p_memsz - p_filesz) */
    if (memsz > filesz) {
        uint8_t *bss_start = (uint8_t *)(vaddr + filesz);
        for (uint64_t i = 0; i < memsz - filesz; i++)
            bss_start[i] = 0;
    }

    obj->segments[obj->seg_count].vaddr  = vaddr;
    obj->segments[obj->seg_count].memsz  = memsz;
    obj->segments[obj->seg_count].filesz = filesz;
    obj->segments[obj->seg_count].prot   = prot;
    obj->seg_count++;

    return 0;
}

/* ── Dynamic section parsing ────────────────────────────────────────── */

static void dl_parse_dynamic(struct dl_loaded_obj *obj,
                              const struct elf64_phdr *dyn_phdr) {
    uint64_t dyn_addr = obj->base_addr + dyn_phdr->p_vaddr;
    uint64_t dyn_size = dyn_phdr->p_memsz;

    if (dyn_size < sizeof(uint64_t) * 2)
        return;

    /* Dynamic entry: { int64_t d_tag; uint64_t d_val; } — 16 bytes */
    const int64_t *dyn = (const int64_t *)dyn_addr;
    int ndyn = (int)(dyn_size / 16);

    for (int i = 0; i < ndyn; i++) {
        int64_t tag = dyn[i * 2];
        uint64_t val = (uint64_t)dyn[i * 2 + 1];

        if (tag == DT_NULL)
            break;

        switch (tag) {
        case DT_SYMTAB:
            obj->dyn_symtab = (val < obj->base_addr) ?
                              (val + obj->base_addr) : val;
            break;
        case DT_STRTAB:
            obj->dyn_strtab = (val < obj->base_addr) ?
                              (val + obj->base_addr) : val;
            break;
        case DT_STRSZ:
            obj->dyn_strsz = val;
            break;
        case DT_SYMENT:
            obj->dyn_syment = val;
            break;
        case DT_INIT:
            obj->init_func = (val < obj->base_addr) ?
                             (val + obj->base_addr) : val;
            break;
        case DT_FINI:
            obj->fini_func = (val < obj->base_addr) ?
                             (val + obj->base_addr) : val;
            break;
        case DT_INIT_ARRAY:
            obj->init_array = (uint64_t *)((val < obj->base_addr) ?
                              (val + obj->base_addr) : val);
            break;
        case DT_INIT_ARRAYSZ:
            obj->init_array_size = val;
            break;
        case DT_FINI_ARRAY:
            obj->fini_array = (uint64_t *)((val < obj->base_addr) ?
                              (val + obj->base_addr) : val);
            break;
        case DT_FINI_ARRAYSZ:
            obj->fini_array_size = val;
            break;
        case DT_RELA:
            obj->rela_dyn = (val < obj->base_addr) ?
                            (val + obj->base_addr) : val;
            break;
        case DT_RELASZ:
            obj->rela_dyn_size = val;
            break;
        case DT_JMPREL:
            obj->rela_plt = (val < obj->base_addr) ?
                            (val + obj->base_addr) : val;
            break;
        case DT_PLTRELSZ:
            obj->rela_plt_size = val;
            break;
        case DT_PLTREL:
            obj->rela_plt_ent = val;
            break;
        case DT_PLTGOT:
            obj->pltgot = (val < obj->base_addr) ?
                           (val + obj->base_addr) : val;
            break;
        case DT_NEEDED:
        case DT_SONAME:
        case DT_RUNPATH:
        case DT_RPATH:
        case DT_FLAGS:
        case DT_TEXTREL:
        case DT_SYMBOLIC:
        case DT_BIND_NOW:
        case DT_DEBUG:
        default:
            break;
        }
    }

    /* Estimate symbol count */
    if (obj->dyn_syment > 0 && obj->dyn_symtab > 0) {
        struct elf64_sym *sym = (struct elf64_sym *)obj->dyn_symtab;
        for (int i = 0; i < 8192; i++) {
            if (sym[i].st_name == 0 && sym[i].st_shndx == 0 &&
                sym[i].st_value == 0 && sym[i].st_info == 0) {
                break;
            }
            obj->dyn_sym_count = i + 1;
        }
    }
}

/* ── Relocation application ─────────────────────────────────────────── */

static void dl_apply_rela_relative(struct dl_loaded_obj *obj,
                                    const struct elf64_rela *rela,
                                    int count) {
    for (int i = 0; i < count; i++) {
        uint32_t r_type = ELF64_R_TYPE(rela[i].r_info);
        if (r_type == R_X86_64_RELATIVE) {
            uint64_t *patch_addr = (uint64_t *)rela[i].r_offset;
            if ((uint64_t)patch_addr < obj->base_addr)
                patch_addr = (uint64_t *)((uint64_t)patch_addr + obj->base_addr);
            *patch_addr = obj->base_addr + rela[i].r_addend;
        }
    }
}

static void dl_apply_rela_glob_dat(struct dl_loaded_obj *obj,
                                    const struct elf64_rela *rela,
                                    int count) {
    for (int i = 0; i < count; i++) {
        uint32_t r_type = ELF64_R_TYPE(rela[i].r_info);
        if (r_type == R_X86_64_GLOB_DAT || r_type == R_X86_64_JUMP_SLOT) {
            uint64_t *patch_addr = (uint64_t *)rela[i].r_offset;
            if ((uint64_t)patch_addr < obj->base_addr)
                patch_addr = (uint64_t *)((uint64_t)patch_addr + obj->base_addr);
            *patch_addr = obj->base_addr + rela[i].r_addend;
        }
    }
}

static void dl_apply_rela_64(struct dl_loaded_obj *obj,
                              const struct elf64_rela *rela,
                              int count) {
    if (!obj->dyn_symtab || !obj->dyn_strtab)
        return;

    struct elf64_sym *sym = (struct elf64_sym *)obj->dyn_symtab;

    for (int i = 0; i < count; i++) {
        uint32_t r_type = ELF64_R_TYPE(rela[i].r_info);
        if (r_type != R_X86_64_64)
            continue;

        uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        if (sym_idx == 0)
            continue;

        uint64_t *patch_addr = (uint64_t *)rela[i].r_offset;
        if ((uint64_t)patch_addr < obj->base_addr)
            patch_addr = (uint64_t *)((uint64_t)patch_addr + obj->base_addr);

        struct elf64_sym *target_sym = &sym[sym_idx];
        uint64_t sym_val = target_sym->st_value;
        if (target_sym->st_shndx != SHN_UNDEF && sym_val < obj->base_addr)
            sym_val += obj->base_addr;

        *patch_addr = sym_val + rela[i].r_addend;
    }
}

static void dl_process_rela(struct dl_loaded_obj *obj) {
    if (obj->rela_dyn == 0 || obj->rela_dyn_size == 0)
        return;

    int count = (int)(obj->rela_dyn_size / sizeof(struct elf64_rela));
    if (count <= 0) return;

    const struct elf64_rela *rela = (const struct elf64_rela *)obj->rela_dyn;
    dl_apply_rela_relative(obj, rela, count);
    dl_apply_rela_glob_dat(obj, rela, count);
    dl_apply_rela_64(obj, rela, count);
}

static void dl_process_rela_plt(struct dl_loaded_obj *obj) {
    if (obj->rela_plt == 0 || obj->rela_plt_size == 0)
        return;

    int count = (int)(obj->rela_plt_size / sizeof(struct elf64_rela));
    if (count <= 0) return;

    const struct elf64_rela *rela = (const struct elf64_rela *)obj->rela_plt;
    dl_apply_rela_glob_dat(obj, rela, count);
}

/* ── init/fini execution ────────────────────────────────────────────── */

static void dl_call_array(uint64_t *array, uint64_t size) {
    if (!array || size == 0) return;
    int count = (int)(size / sizeof(uint64_t));
    for (int i = 0; i < count; i++) {
        if (array[i]) {
            void (*func)(void) = (void (*)(void))array[i];
            func();
        }
    }
}

static void dl_call_init(struct dl_loaded_obj *obj) {
    dl_call_array(obj->init_array, obj->init_array_size);
    if (obj->init_func) {
        void (*func)(void) = (void (*)(void))obj->init_func;
        func();
    }
}

/* ── Symbol lookup ──────────────────────────────────────────────────── */

static void *dl_find_symbol_in_obj(struct dl_loaded_obj *obj,
                                    const char *name) {
    if (!obj->dyn_symtab || !obj->dyn_strtab || !obj->dyn_syment)
        return NULL;

    struct elf64_sym *sym = (struct elf64_sym *)obj->dyn_symtab;
    const char *strtab = (const char *)obj->dyn_strtab;

    for (int i = 0; i < obj->dyn_sym_count; i++) {
        if (sym[i].st_shndx == SHN_UNDEF) continue;
        if (ELF64_ST_BIND(sym[i].st_info) == STB_LOCAL) continue;
        if (sym[i].st_name >= obj->dyn_strsz) continue;

        const char *sym_name = strtab + sym[i].st_name;
        if (strcmp(sym_name, name) == 0) {
            uint64_t addr = sym[i].st_value;
            if (addr < obj->base_addr && addr != 0)
                addr += obj->base_addr;
            return (void *)addr;
        }
    }
    return NULL;
}

static void *dl_find_symbol_global(const char *name) {
    for (int i = handle_count - 1; i >= 0; i--) {
        struct dl_loaded_obj *obj = loaded_handles[i];
        if (!obj) continue;
        void *addr = dl_find_symbol_in_obj(obj, name);
        if (addr) return addr;
    }
    return NULL;
}

/* ── dlopen implementation ──────────────────────────────────────────── */

static uint64_t dl_compute_base(const struct elf64_header *ehdr,
                                 const uint8_t *file_data) {
    (void)file_data;

    uint64_t min_vaddr = ~0ULL;
    uint64_t max_vaddr = 0;
    int has_load = 0;

    const struct elf64_phdr *phdrs = (const struct elf64_phdr *)
        (file_data + ehdr->e_phoff);

    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (phdrs[i].p_vaddr < min_vaddr)
                min_vaddr = phdrs[i].p_vaddr;
            uint64_t end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
            if (end > max_vaddr)
                max_vaddr = end;
            has_load = 1;
        }
    }

    if (!has_load)
        return 0;

    /* Align to 1MB boundary */
    uint64_t align = 0x100000;
    uint64_t base = (dl_next_base + align - 1) & ~(align - 1);

    uint64_t needed = max_vaddr - min_vaddr;
    dl_next_base = base + needed;
    dl_next_base = (dl_next_base + align - 1) & ~(align - 1);

    if (dl_next_base > DL_DEFAULT_BASE + DL_REGION_SIZE)
        dl_next_base = DL_DEFAULT_BASE;

    return base;
}

void *dlopen(const char *filename, int flags) {
    char full_path[256];
    const char *search_path;
    const char *lib_paths[8];
    int num_paths = 0;

    dl_clear_error();

    if (!filename) {
        dl_set_error("dlopen(NULL) is not supported");
        return NULL;
    }

    dl_lock();

    /* Check if already loaded */
    struct dl_loaded_obj *existing = dl_find_loaded(filename);
    if (existing) {
        existing->refcount++;
        dl_unlock();
        return existing->handle;
    }

    search_path = DL_DEFAULT_LIB_PATH;

    /* Split search path */
    full_path[0] = '\0';
    const char *p = search_path;
    while (*p && num_paths < 8) {
        lib_paths[num_paths] = p;
        num_paths++;
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
    }

    /* Try to find and open the file */
    uint8_t *file_data = NULL;
    uint64_t file_size = 0;

    /* Try filename as-is first */
    file_data = dl_read_whole_file(filename, &file_size);
    if (!file_data && num_paths > 0) {
        for (int i = 0; i < num_paths; i++) {
            const char *dir = lib_paths[i];
            const char *end = dir;
            while (*end && *end != ':') end++;

            int dirlen = (int)(end - dir);
            if (dirlen <= 0) continue;

            int j;
            for (j = 0; j < dirlen && j < 200; j++)
                full_path[j] = dir[j];
            if (j > 0 && full_path[j - 1] != '/')
                full_path[j++] = '/';
            int k;
            for (k = 0; filename[k] && j + k < 255; k++)
                full_path[j + k] = filename[k];
            full_path[j + k] = '\0';

            file_data = dl_read_whole_file(full_path, &file_size);
            if (file_data) {
                filename = full_path;
                break;
            }
        }
    }

    if (!file_data) {
        dl_set_error("dlopen: unable to locate library");
        dl_unlock();
        return NULL;
    }

    /* Validate ELF header */
    if (dl_validate_elf(file_data, file_size) < 0) {
        kfree(file_data);
        dl_unlock();
        return NULL;
    }

    const struct elf64_header *ehdr = (const struct elf64_header *)file_data;

    /* Allocate loaded-obj descriptor */
    struct dl_loaded_obj *obj = (struct dl_loaded_obj *)
        kmalloc(sizeof(struct dl_loaded_obj));
    if (!obj) {
        kfree(file_data);
        dl_set_error("dlopen: out of memory");
        dl_unlock();
        return NULL;
    }
    /* Zero-fill the descriptor */
    {
        uint8_t *b = (uint8_t *)obj;
        for (size_t i = 0; i < sizeof(struct dl_loaded_obj); i++)
            b[i] = 0;
    }

    obj->magic = DL_MAGIC;
    obj->handle = (void *)obj;
    obj->flags = flags;
    obj->refcount = 1;

    /* Store library name */
    {
        int name_len = strlen(filename);
        if (name_len >= DL_OBJ_MAX_NAME) name_len = DL_OBJ_MAX_NAME - 1;
        int i;
        for (i = 0; i < name_len; i++)
            obj->name[i] = filename[i];
        obj->name[name_len] = '\0';
    }

    /* Compute load base */
    obj->base_addr = dl_compute_base(ehdr, file_data);

    /* Parse program headers */
    const struct elf64_phdr *phdrs = (const struct elf64_phdr *)
        (file_data + ehdr->e_phoff);
    const struct elf64_phdr *dyn_phdr = NULL;
    int pt_load_count = 0;

    for (int i = 0; i < (int)ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            if (dl_map_segment(obj, file_data, &phdrs[i]) < 0) {
                kfree(file_data);
                dl_unlock();
                return NULL;
            }
            pt_load_count++;
        } else if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_phdr = &phdrs[i];
        }
    }

    if (pt_load_count == 0) {
        kfree(file_data);
        kfree(obj);
        dl_set_error("dlopen: no loadable segments");
        dl_unlock();
        return NULL;
    }

    /* Parse dynamic section */
    if (dyn_phdr) {
        dl_parse_dynamic(obj, dyn_phdr);
    }

    kfree(file_data);
    file_data = NULL;

    /* Apply relocations */
    dl_process_rela(obj);
    dl_process_rela_plt(obj);

    /* Register in global list */
    if (dl_register(obj) < 0) {
        kfree(obj);
        dl_set_error("dlopen: too many loaded libraries");
        dl_unlock();
        return NULL;
    }

    obj->loaded = 1;

    /* Call init functions */
    dl_call_init(obj);

    dl_unlock();
    return obj->handle;
}

/* ── dlsym implementation ───────────────────────────────────────────── */

void *dlsym(void *handle, const char *symbol) {
    if (!symbol) {
        dl_set_error("dlsym: NULL symbol name");
        return NULL;
    }

    dl_clear_error();
    dl_lock();

    void *result = NULL;

    if (handle == RTLD_DEFAULT) {
        result = dl_find_symbol_global(symbol);
    } else if (handle == RTLD_NEXT) {
        result = dl_find_symbol_global(symbol);
    } else {
        struct dl_loaded_obj *obj = (struct dl_loaded_obj *)handle;
        int found = 0;

        for (int i = 0; i < handle_count; i++) {
            if (loaded_handles[i] == obj) {
                found = 1;
                break;
            }
        }

        if (!found) {
            dl_set_error("dlsym: invalid handle");
            dl_unlock();
            return NULL;
        }

        result = dl_find_symbol_in_obj(obj, symbol);

        if (!result && (obj->flags & RTLD_GLOBAL)) {
            result = dl_find_symbol_global(symbol);
        }
    }

    dl_unlock();
    return result;
}

/* ── dlclose implementation ─────────────────────────────────────────── */

int dlclose(void *handle) {
    if (!handle) {
        dl_set_error("dlclose: NULL handle");
        return -1;
    }

    dl_clear_error();
    dl_lock();

    struct dl_loaded_obj *obj = (struct dl_loaded_obj *)handle;

    int found_idx = -1;
    for (int i = 0; i < handle_count; i++) {
        if (loaded_handles[i] == obj) {
            found_idx = i;
            break;
        }
    }

    if (found_idx < 0) {
        dl_set_error("dlclose: invalid handle");
        dl_unlock();
        return -1;
    }

    obj->refcount--;
    if (obj->refcount > 0) {
        dl_unlock();
        return 0;
    }

    /* Call fini functions */
    if (obj->fini_func) {
        void (*func)(void) = (void (*)(void))obj->fini_func;
        func();
    }
    dl_call_array(obj->fini_array, obj->fini_array_size);

    /* Unload dependencies */
    for (int i = 0; i < obj->dep_count; i++) {
        if (obj->deps[i]) {
            dlclose(obj->deps[i]->handle);
        }
    }

    /* Unmap all segments */
    for (int i = 0; i < obj->seg_count; i++) {
        uint64_t vaddr = obj->segments[i].vaddr & ~DL_PAGE_MASK;
        uint64_t memsz = obj->segments[i].memsz;
        uint64_t page_off = obj->segments[i].vaddr & DL_PAGE_MASK;
        uint64_t map_size = memsz + page_off;
        map_size = (map_size + DL_PAGE_SIZE - 1) & ~DL_PAGE_MASK;
        if (map_size > 0) {
            dl_munmap(vaddr, map_size);
        }
    }

    dl_unregister(handle);
    kfree(obj);

    dl_unlock();
    return 0;
}

/* ── dlvsym — versioned symbol lookup (GNU extension) ────────────────── */
static void *dlvsym(void *handle, const char *symbol, const char *version)
{
    (void)handle;
    (void)symbol;
    (void)version;
    dl_set_error("dlvsym: not supported");
    return NULL;
}
