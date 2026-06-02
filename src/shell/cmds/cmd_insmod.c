/* cmd_insmod.c — insmod: load a kernel module (.ko file)
 *
 * M21: insmod shell command for the modular kernel transition.
 *
 * Usage: insmod <path> [param=val ...]
 *
 * Reads the .ko file via VFS, then calls the ELF module loader
 * (module_elf_validate -> module_elf_parse -> module_elf_finalize)
 * to load and initialize the module.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"
#include "heap.h"
#include "module.h"
#include "module_elf.h"

void cmd_insmod(const char *args)
{
    if (!args || !args[0]) {
        kprintf("Usage: insmod <path> [param=val ...]\n");
        return;
    }

    /* Extract the first space-delimited token as the path */
    char path[128];
    const char *p = args;
    char *d = path;
    while (*p && *p != ' ' && (size_t)(d - path) < sizeof(path) - 1) {
        *d++ = *p++;
    }
    *d = '\0';

    if (path[0] == '\0') {
        kprintf("insmod: missing path\n");
        return;
    }

    /* Skip params for now (module params parsing is M29+) */
    /* while (*p == ' ') p++; */

    /* Stat the file to get its size */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0) {
        kprintf("insmod: cannot stat '%s'\n", path);
        return;
    }

    uint64_t file_size = st.size;
    if (file_size == 0 || file_size > 8 * 1024 * 1024) {
        kprintf("insmod: '%s' has bad size (%llu)\n", path,
                (unsigned long long)file_size);
        return;
    }

    /* Allocate a buffer for the file contents */
    void *buf = kmalloc((size_t)file_size);
    if (!buf) {
        kprintf("insmod: out of memory (%llu bytes)\n",
                (unsigned long long)file_size);
        return;
    }

    /* Read the file */
    uint32_t bytes_read = 0;
    if (vfs_read(path, buf, (uint32_t)file_size, &bytes_read) < 0 ||
        bytes_read != file_size) {
        kprintf("insmod: failed to read '%s'\n", path);
        kfree(buf);
        return;
    }

    /* Run the ELF module loader */
    struct module_elf_context ctx;
    int result;

    /* Step 1: Validate ELF header */
    result = module_elf_validate(&ctx, (const uint8_t *)buf, file_size);
    if (result < 0) {
        kprintf("insmod: '%s' validation failed: %s\n", path, ctx.error_msg);
        kfree(buf);
        return;
    }

    /* Step 2: Parse ELF sections, symbols, relocations */
    result = module_elf_parse(&ctx);
    if (result < 0) {
        kprintf("insmod: '%s' parse failed: %s\n", path, ctx.error_msg);
        kfree(buf);
        return;
    }

    /* Step 3: Finalize (resolve, load, relocate, set perms, call init) */
    const char *mod_name = ctx.name;
    if (mod_name[0] == '\0') {
        /* Fall back to filename (without path) */
        const char *slash = path;
        const char *last = path;
        while (*slash) {
            if (*slash == '/') last = slash + 1;
            slash++;
        }
        mod_name = last;
    }

    result = module_elf_finalize(&ctx, mod_name);
    module_elf_free(&ctx);
    kfree(buf);

    if (result < 0) {
        kprintf("insmod: '%s' loading failed: %s\n", path, ctx.error_msg);
    } else {
        kprintf("insmod: Loaded '%s' (module id %d)\n", path, result);
    }
}
