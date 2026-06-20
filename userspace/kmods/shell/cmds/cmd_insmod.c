/* cmd_insmod.c — Load a kernel module from a .ko file (M21) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"
#include "heap.h"
#include "module.h"
#include "module_elf.h"
#include "errno.h"

void cmd_insmod(const char *args) {
    /* Parse args: first word is the path, rest is params (optional) */
    if (!args || !*args) {
        kprintf("Usage: insmod <path> [param=val ...]\n");
        return;
    }

    /* Skip leading whitespace */
    while (*args == ' ') args++;
    if (!*args) {
        kprintf("Usage: insmod <path> [param=val ...]\n");
        return;
    }

    /* Extract path (first word) and optional params (rest) */
    char path[128];
    const char *params = NULL;
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < (int)sizeof(path) - 1) {
        path[i++] = *p++;
    }
    path[i] = '\0';
    /* Skip whitespace to find params */
    while (*p == ' ') p++;
    if (*p) params = p;

    /* Stat the .ko file */
    struct vfs_stat st;
    if (vfs_stat(path, &st) < 0) {
        kprintf("insmod: file not found: %s\n", path);
        return;
    }

    uint64_t file_size = st.size;
    if (file_size == 0 || file_size > 8 * 1024 * 1024) {
        kprintf("insmod: invalid file size %llu\n", (unsigned long long)file_size);
        return;
    }

    /* Read the file into a kernel buffer */
    void *buf = kmalloc((size_t)file_size);
    if (!buf) {
        kprintf("insmod: out of memory\n");
        return;
    }

    uint32_t bytes_read = 0;
    int ret = vfs_read(path, buf, (uint32_t)file_size, &bytes_read);
    if (ret < 0 || bytes_read != file_size) {
        kprintf("insmod: read failed (ret=%d, read=%u/%llu)\n",
                ret, (unsigned)bytes_read, (unsigned long long)file_size);
        kfree(buf);
        return;
    }

    /* Run the ELF module loader */
    struct module_elf_context ctx;
    int result = -1;

    /* Validate ELF header */
    if (module_elf_validate(&ctx, (const uint8_t *)buf, file_size) < 0) {
        kprintf("insmod: ELF validation failed: %s\n", ctx.error_msg);
        kfree(buf);
        return;
    }

    /* Parse ELF sections, symbols, relocations */
    if (module_elf_parse(&ctx) < 0) {
        kprintf("insmod: ELF parse failed: %s\n", ctx.error_msg);
        kfree(buf);
        return;
    }

    /* Finalize: resolve symbols, load sections, apply relocations,
     * set permissions, call init function */
    result = module_elf_finalize(&ctx, ctx.name[0] ? ctx.name : path);
    module_elf_free(&ctx);
    kfree(buf);

    if (result < 0) {
        kprintf("insmod: failed to load module: %s\n", ctx.error_msg);
        return;
    }

    /* Parse module parameters (if any) */
    if (params && params[0]) {
        struct kernel_module *mod = module_get_by_id(result);
        if (mod) {
            int pret = module_parse_params(mod, params);
            if (pret < 0) {
                kprintf("insmod: parameter parsing failed (%d), unloading\n", pret);
                module_unload(result);
                return;
            }
            /* Create sysfs entries for parameters */
            module_sysfs_add_params(mod);
        }
    } else {
        /* Even without parameters, create sysfs entries */
        struct kernel_module *mod = module_get_by_id(result);
        if (mod)
            module_sysfs_add_params(mod);
    }

    kprintf("insmod: loaded '%s' as id=%d\n",
            ctx.name[0] ? ctx.name : path, result);
}
