/* cmd_depmod.c — module dependency analysis and modules.dep generator
 *
 * Scans /modules/ for .ko files, reads each module's .modinfo section,
 * extracts dependency declarations, and writes /modules/modules.dep.
 *
 * The output format matches Linux conventions:
 *   module_name.ko: dep1.ko dep2.ko
 *
 * Usage:
 *   depmod              — scan /modules/ and regenerate modules.dep
 *   depmod -a           — same (all modules)
 *   depmod <path>       — scan a specific directory
 */
#include "shell_cmds.h"
#include "libc.h"       /* vfs_read, vfs_write, fs_list_names, vfs_stat */
#include "printf.h"
#include "string.h"
#include "stdlib.h"     /* malloc/free, qsort */
#include "types.h"

/* ── Constants ─────────────────────────────────────────────────────────── */
#define KO_FILE_MAX      128           /* max .ko files we can track      */
#define KO_NAME_MAX      64            /* max module name length          */
#define DEP_STRING_MAX   1024          /* max dependency string per module */
#define MAX_PATH         256           /* max path length                 */
#define BUFFER_SIZE      (128 * 1024)  /* max .ko file size to read      */
#define FILENAME_MAX_LEN 28            /* matches FS_MAX_NAME from libc.h */

/* Output: one entry per module */
struct dep_entry {
    char   name[KO_NAME_MAX];      /* module name (without .ko)      */
    char   deps[DEP_STRING_MAX];   /* space-separated dependency list */
};

/* Track all modules found and their dependencies */
static struct dep_entry dep_table[KO_FILE_MAX];
static int              dep_count = 0;

/* ── Minimal ELF structures for on-disk modinfo parsing ────────────────── */
/* These are local definitions so we don't need to include the full elf.h. */

#define EI_NIDENT       16
#define EI_CLASS        4
#define ELFCLASS64      2
#define ET_REL          1
#define EM_X86_64       62

struct __attribute__((packed)) depmod_elf64_hdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
};

struct __attribute__((packed)) depmod_elf64_shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

#define DEPMOD_ELF_MAGIC  0x464C457F  /* "\x7fELF" in LE */

/* ── Parse modinfo data ───────────────────────────────────────────────────
 *
 * The .modinfo section is a sequence of NUL-terminated "key=value" strings.
 * We extract "name=" and "depends=" fields.
 */
static int parse_modinfo(const uint8_t *data, uint64_t size,
                          char *name_out, int name_max,
                          char *deps_out, int deps_max)
{
    int found_name = 0;
    int found_deps = 0;

    if (name_out)  *name_out  = '\0';
    if (deps_out)  *deps_out  = '\0';

    uint64_t pos = 0;
    while (pos < size) {
        const char *entry = (const char *)(data + pos);
        uint64_t remain = size - pos;
        uint64_t elen = 0;

        /* Find NUL terminator */
        while (elen < remain && entry[elen] != '\0')
            elen++;

        if (elen == 0) {
            pos++;  /* skip empty entry */
            continue;
        }

        /* Check for "name=" */
        if (name_out && !found_name &&
            strncmp(entry, "name=", 5) == 0) {
            const char *val = entry + 5;
            int vlen = (int)(elen - 5);
            if (vlen > name_max - 1) vlen = name_max - 1;
            memcpy(name_out, val, (size_t)vlen);
            name_out[vlen] = '\0';
            found_name = 1;
        }

        /* Check for "depends=" */
        if (deps_out && !found_deps &&
            strncmp(entry, "depends=", 8) == 0) {
            const char *val = entry + 8;
            int vlen = (int)(elen - 8);
            if (vlen > deps_max - 1) vlen = deps_max - 1;
            memcpy(deps_out, val, (size_t)vlen);
            deps_out[vlen] = '\0';
            found_deps = 1;
        }

        /* Skip to next entry */
        pos += elen + 1;
    }

    return (found_name || found_deps) ? 0 : -1;
}

/* ── Parse a .ko file ─────────────────────────────────────────────────────
 *
 * Read the module data from VFS, find the .modinfo section, and
 * extract dependency information.
 */
static int parse_ko_file(const char *path, const char *filename)
{
    uint8_t *buf;
    uint32_t size;
    int ret;

    /* Read the .ko file into a heap buffer */
    buf = (uint8_t *)malloc(BUFFER_SIZE);
    if (!buf) {
        kprintf("depmod: out of memory reading %s\n", filename);
        return -1;
    }

    ret = vfs_read(path, buf, BUFFER_SIZE, &size);
    if (ret < 0 || size < sizeof(struct depmod_elf64_hdr)) {
        kprintf("depmod: cannot read %s (error=%d)\n", path, ret);
        free(buf);
        return -1;
    }

    /* Validate ELF header */
    struct depmod_elf64_hdr *hdr = (struct depmod_elf64_hdr *)buf;
    if (*(const uint32_t *)hdr->e_ident != DEPMOD_ELF_MAGIC ||
        hdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        hdr->e_machine != EM_X86_64 ||
        hdr->e_type != ET_REL) {
        /* Not a valid kernel module — skip silently */
        free(buf);
        return 0;
    }

    /* Locate section headers */
    if (hdr->e_shentsize != sizeof(struct depmod_elf64_shdr) ||
        hdr->e_shoff == 0 || hdr->e_shnum == 0 ||
        hdr->e_shstrndx >= hdr->e_shnum) {
        free(buf);
        return 0;
    }

    /* Compute safe number of sections */
    uint64_t end_of_sections = hdr->e_shoff +
        (uint64_t)hdr->e_shnum * (uint64_t)hdr->e_shentsize;
    if (end_of_sections > (uint64_t)size) {
        free(buf);
        return 0;
    }

    /* Find section name string table */
    struct depmod_elf64_shdr *shstrtab_hdr =
        (struct depmod_elf64_shdr *)(buf + hdr->e_shoff +
                              (uint64_t)hdr->e_shstrndx * (uint64_t)hdr->e_shentsize);
    const char *shstrtab = (const char *)(buf + shstrtab_hdr->sh_offset);
    uint64_t shstrtab_size = shstrtab_hdr->sh_size;

    /* Validate shstrtab bounds */
    if (shstrtab_hdr->sh_offset + shstrtab_size > (uint64_t)size) {
        free(buf);
        return 0;
    }

    /* Scan sections for .modinfo */
    int modinfo_found = 0;
    const uint8_t *modinfo_data = NULL;
    uint64_t modinfo_size = 0;

    for (int i = 0; i < hdr->e_shnum; i++) {
        struct depmod_elf64_shdr *sh = (struct depmod_elf64_shdr *)
            (buf + hdr->e_shoff + (uint64_t)i * (uint64_t)hdr->e_shentsize);

        /* Get section name from string table */
        const char *sname = "";
        if (sh->sh_name < shstrtab_size)
            sname = shstrtab + sh->sh_name;

        if (strcmp(sname, ".modinfo") == 0) {
            if (sh->sh_offset + sh->sh_size > (uint64_t)size)
                break;  /* invalid offset */
            modinfo_data = buf + sh->sh_offset;
            modinfo_size = sh->sh_size;
            modinfo_found = 1;
            break;
        }
    }

    if (!modinfo_found) {
        free(buf);
        return 0;  /* no modinfo — not a kernel module */
    }

    /* Extract name and depends from modinfo */
    char mod_name[KO_NAME_MAX];
    char mod_deps[DEP_STRING_MAX];

    if (parse_modinfo(modinfo_data, modinfo_size,
                      mod_name, sizeof(mod_name),
                      mod_deps, sizeof(mod_deps)) < 0) {
        free(buf);
        return 0;
    }

    /* If no name found, derive from filename */
    if (mod_name[0] == '\0') {
        const char *dot = strstr(filename, ".ko");
        int nlen = dot ? (int)(dot - filename) : (int)strlen(filename);
        if (nlen > KO_NAME_MAX - 1) nlen = KO_NAME_MAX - 1;
        memcpy(mod_name, filename, (size_t)nlen);
        mod_name[nlen] = '\0';
    }

    /* Store in dep table (replace existing entry if same name) */
    int slot = -1;
    for (int i = 0; i < dep_count; i++) {
        if (strcmp(dep_table[i].name, mod_name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (dep_count >= KO_FILE_MAX) {
            free(buf);
            return 0;
        }
        slot = dep_count++;
    }

    strncpy(dep_table[slot].name, mod_name, KO_NAME_MAX - 1);
    dep_table[slot].name[KO_NAME_MAX - 1] = '\0';
    strncpy(dep_table[slot].deps, mod_deps, DEP_STRING_MAX - 1);
    dep_table[slot].deps[DEP_STRING_MAX - 1] = '\0';

    free(buf);
    return 0;
}

/* ── Write modules.dep ────────────────────────────────────────────────────
 *
 * Output format (Linux-compatible):
 *   module_name.ko: dep1.ko dep2.ko
 *
 * Modules with no dependencies get a line with no deps after the colon.
 */
static int write_modules_dep(const char *output_dir)
{
    char dep_path[MAX_PATH];
    int len = snprintf(dep_path, sizeof(dep_path),
                       "%s/modules.dep", output_dir);
    if (len < 0 || len >= (int)sizeof(dep_path)) {
        kprintf("depmod: output path too long\n");
        return -1;
    }

    /* Build output content in memory */
    char out_buf[16384];
    int pos = 0;

    for (int i = 0; i < dep_count; i++) {
        int n = snprintf(out_buf + pos, (size_t)(sizeof(out_buf) - pos),
                         "%s.ko:", dep_table[i].name);
        if (n > 0) pos += n;
        if (pos >= (int)sizeof(out_buf)) break;

        /* Add dependencies (converting comma-separated to space-separated, .ko suffix) */
        if (dep_table[i].deps[0] != '\0') {
            char deps_copy[DEP_STRING_MAX];
            strncpy(deps_copy, dep_table[i].deps, DEP_STRING_MAX - 1);
            deps_copy[DEP_STRING_MAX - 1] = '\0';

            char *saveptr;
            char *tok = strtok_r(deps_copy, ",", &saveptr);
            while (tok) {
                /* Skip leading spaces */
                while (*tok == ' ') tok++;
                if (*tok == '\0') {
                    tok = strtok_r(NULL, ",", &saveptr);
                    continue;
                }

                int m = snprintf(out_buf + pos,
                                 (size_t)(sizeof(out_buf) - pos),
                                 " %s.ko", tok);
                if (m > 0) pos += m;
                if (pos >= (int)sizeof(out_buf)) break;

                tok = strtok_r(NULL, ",", &saveptr);
            }
        }

        int nl = snprintf(out_buf + pos,
                          (size_t)(sizeof(out_buf) - pos), "\n");
        if (nl > 0) pos += nl;
        if (pos >= (int)sizeof(out_buf)) break;
    }

    /* Write the output file */
    if (pos == 0) {
        kprintf("depmod: no modules found, not writing modules.dep\n");
        return -1;
    }

    int ret = vfs_write(dep_path, out_buf, (uint32_t)pos);
    if (ret < 0) {
        kprintf("depmod: failed to write %s (error=%d)\n", dep_path, ret);
        return -1;
    }

    kprintf("depmod: wrote %s (%d bytes, %d modules)\n",
            dep_path, pos, dep_count);
    return 0;
}

/* ── Scan directory for .ko files ──────────────────────────────────────── */
static int scan_directory(const char *dir_path)
{
    char names[KO_FILE_MAX][FILENAME_MAX_LEN];
    int count;

    /* Use libc's directory listing (lists all entries, no prefix filter) */
    count = fs_list_names(dir_path, "", names, KO_FILE_MAX);
    if (count < 0) {
        kprintf("depmod: cannot read directory '%s' (error=%d)\n",
                dir_path, count);
        return -1;
    }

    kprintf("depmod: scanning %s (%d entries)...\n", dir_path, count);

    /* Reset the dep table */
    dep_count = 0;

    /* Process each .ko file */
    int processed = 0;
    for (int i = 0; i < count; i++) {
        /* Check for .ko extension */
        const char *name = names[i];
        int nlen = (int)strlen(name);

        if (nlen <= 3 ||
            name[nlen - 3] != '.' ||
            name[nlen - 2] != 'k' ||
            name[nlen - 1] != 'o') {
            continue;  /* not a .ko file */
        }

        /* Build full path */
        char full_path[MAX_PATH];
        int plen = snprintf(full_path, sizeof(full_path),
                            "%s/%s", dir_path, name);
        if (plen < 0 || plen >= (int)sizeof(full_path))
            continue;

        if (parse_ko_file(full_path, name) == 0)
            processed++;
    }

    kprintf("depmod: processed %d .ko file(s), %d module(s) registered\n",
            processed, dep_count);
    return processed;
}

/* ── depmod command entry point ───────────────────────────────────────── */
int cmd_depmod(int argc, char **argv)
{
    const char *target_dir = "/modules";

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            /* Use default /modules directory */
            continue;
        } else if (argv[i][0] == '-') {
            kprintf("depmod: unknown option '%s'\n", argv[i]);
            kprintf("Usage: depmod [-a] [directory]\n");
            return 1;
        } else {
            target_dir = argv[i];
        }
    }

    kprintf("depmod: module dependency analysis\n");

    /* Scan the target directory */
    int processed = scan_directory(target_dir);
    if (processed < 0)
        return 1;

    if (processed == 0) {
        kprintf("depmod: no .ko files found in '%s'\n", target_dir);
        return 0;
    }

    /* Write modules.dep */
    if (write_modules_dep(target_dir) < 0)
        return 1;

    return 0;
}

void depmod_init(void)
{
    /* No special init needed; the command is self-contained */
    kprintf("[OK] depmod: module dependency analysis available\n");
}
