/* modinfo.c — show module information from .ko file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Parse modinfo section from a .ko ELF file and print key=value pairs */
static int show_modinfo(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    /* Read the entire .ko file (up to 512KB) */
    char *buf = malloc(524288);
    if (!buf) {
        close(fd);
        return -1;
    }

    int n = read(fd, buf, 524288);
    close(fd);
    if (n <= 0) {
        free(buf);
        return -1;
    }

    /* Search for modinfo magic string which appears as null-terminated key=value pairs */
    /* The modinfo section contains strings like: "author=John\0description=...\0" */
    const char *p = buf;
    const char *end = buf + n;

    /* Search for known modinfo keys */
    const char *keys[] = {
        "author", "description", "alias", "license",
        "depends", "parm", "parmtype", "version",
        "name", "firmware", "srcversion", "intree",
        "vermagic", NULL
    };

    int found = 0;
    while (p < end) {
        /* Skip null bytes */
        while (p < end && *p == 0) p++;
        if (p >= end) break;

        /* Check if this string starts with any known key */
        for (int i = 0; keys[i]; i++) {
            unsigned long klen = strlen(keys[i]);
            if (p + klen < end && memcmp(p, keys[i], klen) == 0 && p[klen] == '=') {
                /* Found a key=value pair */
                const char *val = p + klen + 1;
                unsigned long vlen = 0;
                while (val + vlen < end && val[vlen] != 0) vlen++;

                printf("%s: ", keys[i]);
                write(1, val, vlen);
                printf("\n");
                found = 1;
                break;
            }
        }

        /* Skip to next null */
        while (p < end && *p != 0) p++;
    }

    free(buf);

    if (!found) {
        printf("modinfo: no modinfo section found in '%s'\n", path);
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: modinfo MODULE|MODULE.ko\n");
        return 1;
    }

    const char *arg = argv[1];
    char fullpath[256];

    /* Check if it has .ko extension or if we should look in /modules/ */
    unsigned long len = strlen(arg);
    if (len > 3 && strcmp(arg + len - 3, ".ko") == 0) {
        /* Direct path */
        if (show_modinfo(arg) < 0) {
            printf("modinfo: cannot open '%s'\n", arg);
            return 1;
        }
    } else {
        /* Try /modules/<module>.ko */
        snprintf(fullpath, sizeof(fullpath), "/modules/%s.ko", arg);
        if (show_modinfo(fullpath) < 0) {
            /* Try /modules/<module>/module.ko */
            snprintf(fullpath, sizeof(fullpath), "/modules/%s/module.ko", arg);
            if (show_modinfo(fullpath) < 0) {
                printf("modinfo: module '%s' not found\n", arg);
                return 1;
            }
        }
    }

    return 0;
}
