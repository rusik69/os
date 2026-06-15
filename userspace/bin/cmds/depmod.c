/* depmod.c — generate module dependency file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MODULES_DIR "/modules/"

/* Simple modinfo section reader */
static int read_modinfo(const char *path, char *buf, unsigned long bufsz) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    /* Read .ko file and find modinfo section */
    char filebuf[65536];
    int n = read(fd, filebuf, sizeof(filebuf));
    close(fd);
    if (n <= 0) return -1;

    /* Look for "depends=" in modinfo */
    const char *dep_tag = "depends=";
    const char *p = filebuf;
    int remaining = n;

    while (remaining > 0) {
        const char *found = strstr(p, dep_tag);
        if (!found) break;

        found += strlen(dep_tag);
        /* Copy dependencies until null byte or end */
        unsigned long out = 0;
        while (out < bufsz - 1 && found < filebuf + n && *found && *found != '\n') {
            buf[out++] = *found;
            found++;
        }
        buf[out] = '\0';
        return (int)out;
    }

    buf[0] = '\0';
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Open /modules directory */
    int dirfd = open(MODULES_DIR, O_RDONLY, 0);
    if (dirfd < 0) {
        printf("depmod: cannot open %s\n", MODULES_DIR);
        return 1;
    }

    /* Create modules.dep file */
    int outfd = open("/modules/modules.dep", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
        printf("depmod: cannot create /modules/modules.dep\n");
        close(dirfd);
        return 1;
    }

    /* Read directory entries */
    char buf[4096];
    int n;
    while ((n = getdents64(dirfd, buf, sizeof(buf))) > 0) {
        struct dirent *d;
        int pos = 0;
        while (pos < n) {
            d = (struct dirent *)(buf + pos);
            pos += d->d_reclen;

            /* Skip . and .. */
            if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
                continue;

            /* Check if it's a .ko file */
            unsigned long len = strlen(d->d_name);
            if (len < 4 || strcmp(d->d_name + len - 3, ".ko") != 0)
                continue;

            /* Build full path */
            char fullpath[256];
            snprintf(fullpath, sizeof(fullpath), "%s%s", MODULES_DIR, d->d_name);

            /* Read dependencies from modinfo */
            char deps[512];
            read_modinfo(fullpath, deps, sizeof(deps));

            /* Write to modules.dep: module.ko: dep1.ko dep2.ko ... */
            if (deps[0]) {
                /* Write: module.ko: dep1.ko dep2.ko\n */
                write(outfd, d->d_name, strlen(d->d_name));
                write(outfd, ": ", 2);
                write(outfd, deps, strlen(deps));
                write(outfd, "\n", 1);
            } else {
                /* No dependencies */
                write(outfd, d->d_name, strlen(d->d_name));
                write(outfd, ":\n", 2);
            }
        }
    }

    close(dirfd);
    close(outfd);

    printf("depmod: generated /modules/modules.dep\n");
    return 0;
}
