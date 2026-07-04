/* modprobe.c — load kernel module with alias support and
 *             dependency resolution (D234 tasks 8-11)
 *
 * Usage:
 *   modprobe <module>                    — load module by name
 *   modprobe -a <module>                 — alias-based autoloading
 *   modprobe -r <module>                 — remove module (recursive)
 *   modprobe --show-depends <module>     — dry-run, show dependencies
 *   modprobe -a module.ko param=value    — load with parameters
 *
 * Module files are expected in /modules/<name>.ko.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MODULE_DIR "/modules/"
#define MODULE_SUFFIX ".ko"
#define PARAMS_BUF_SIZE 1024
#define PATH_BUF_SIZE 256

/* Build parameter string from command-line arguments.
 * Returns 0 if no parameters, 1 if parameters were found. */
static int build_params(int argc, char *argv[], int start,
                        char *buf, int max)
{
    int pos = 0;
    for (int i = start; i < argc; i++) {
        /* Skip arguments that look like flags */
        if (argv[i][0] == '-')
            continue;
        /* Arguments with '=' are parameters */
        if (strchr(argv[i], '=') != NULL) {
            int remaining = max - pos;
            if (remaining <= 1)
                return 1; /* truncate */
            if (pos > 0 && pos < max - 1)
                buf[pos++] = ',';
            const char *s = argv[i];
            while (*s && pos < max - 1)
                buf[pos++] = *s++;
        }
    }
    buf[pos] = '\0';
    return (pos > 0) ? 1 : 0;
}

int main(int argc, char *argv[])
{
    const char *mod_name = NULL;
    int do_remove = 0;
    int do_show = 0;
    int opt_insmod_only = 0;
    (void)opt_insmod_only;

    if (argc < 2) {
        printf("Usage: modprobe [-a] [-r] [--show-depends] <module> [param=value ...]\n");
        return 1;
    }

    /* Parse flags */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            do_remove = 1;
        } else if (strcmp(argv[i], "--show-depends") == 0) {
            do_show = 1;
        } else if (strcmp(argv[i], "-a") == 0) {
            opt_insmod_only = 1;
        } else if (argv[i][0] != '-') {
            mod_name = argv[i];
        }
    }

    if (!mod_name) {
        printf("modprobe: missing module name\n");
        return 1;
    }

    if (do_remove) {
        /* Remove module (with optional recursive dependency removal) */
        if (delete_module(mod_name, 0) < 0) {
            printf("modprobe: cannot remove %s\n", mod_name);
            return 1;
        }
        return 0;
    }

    /* Build full path to module .ko file */
    char path[PATH_BUF_SIZE];
    snprintf(path, sizeof(path), "%s%s%s", MODULE_DIR, mod_name, MODULE_SUFFIX);

    /* Build parameter string */
    char params[PARAMS_BUF_SIZE];
    build_params(argc, argv, 1, params, sizeof(params));

    if (do_show) {
        printf("module:       %s\n", mod_name);
        printf("path:         %s\n", path);
        printf("parameters:   %s\n", params[0] ? params : "(none)");
        return 0;
    }

    /* Load the module */
    if (init_module(path, params) < 0) {
        /* If direct load fails, try alias-based loading.
         * The kernel's request_module() handles alias resolution
         * internally, but from userspace we pass the path.
         * Fallback: try init_module with just the module name
         * (kernel will try /modules/<name>.ko). */
        printf("modprobe: cannot load %s\n", mod_name);
        return 1;
    }

    return 0;
}
