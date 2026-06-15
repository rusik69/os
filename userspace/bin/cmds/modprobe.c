/* modprobe.c — Load kernel module with dependency resolution */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MODULES_DIR "/modules/"

static int load_module(const char *path) {
    int ret = init_module(path, NULL);
    if (ret < 0) {
        /* init_module returns module_id on success, negative on error */
        return ret;
    }
    return 0;
}

static int load_with_deps(const char *module_name);

static int resolve_and_load(const char *dep) {
    /* Strip .ko suffix if present */
    char name[64];
    unsigned long len = strlen(dep);
    unsigned long copy_len = len;
    if (len > 3 && strcmp(dep + len - 3, ".ko") == 0)
        copy_len = len - 3;
    if (copy_len > sizeof(name) - 1)
        copy_len = sizeof(name) - 1;
    memcpy(name, dep, copy_len);
    name[copy_len] = '\0';
    return load_with_deps(name);
}

static int load_with_deps(const char *module_name) {
    /* Build path: /modules/<module>.ko */
    char path[256];
    snprintf(path, sizeof(path), "%s%s.ko", MODULES_DIR, module_name);

    /* Check if file exists */
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("modprobe: module '%s' not found\n", module_name);
        return 1;
    }

    /* Read module's dep info from modinfo in the .ko file */
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 1;

    char buf[65536];
    int n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n > 0) {
        /* Look for "depends=" tag */
        const char *dep_tag = "depends=";
        const char *p = buf;
        int remaining = n;

        while (remaining > 0) {
            const char *found = strstr(p, dep_tag);
            if (!found) break;

            found += strlen(dep_tag);
            char deps[256];
            unsigned int dpos = 0;

            while (dpos < sizeof(deps) - 1 && found < buf + n && *found && *found != '\n') {
                deps[dpos++] = *found;
                found++;
            }
            deps[dpos] = '\0';

            if (deps[0]) {
                /* Parse comma-separated dependencies */
                char *tok = deps;
                char *comma;
                while ((comma = strchr(tok, ',')) != NULL) {
                    *comma = '\0';
                    /* Trim spaces */
                    char *start = tok;
                    while (*start == ' ') start++;
                    if (*start) {
                        printf("modprobe: loading dependency '%s'...\n", start);
                        resolve_and_load(start);
                    }
                    tok = comma + 1;
                }
                /* Last token */
                char *start = tok;
                while (*start == ' ') start++;
                if (*start) {
                    printf("modprobe: loading dependency '%s'...\n", start);
                    resolve_and_load(start);
                }
            }
            break;
        }
    }

    /* Now load the module itself */
    printf("modprobe: loading '%s'...\n", module_name);
    if (load_module(path) < 0) {
        printf("modprobe: failed to load '%s'\n", module_name);
        return 1;
    }

    printf("modprobe: loaded '%s'\n", module_name);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: modprobe MODULE [args...]\n");
        return 1;
    }

    return load_with_deps(argv[1]);
}
