/* find.c — recursive directory search by name pattern */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

static int found = 0;

static void search_dir(const char *dir, const char *pattern) {
    int fd = open(dir, O_RDONLY, 0);
    if (fd < 0) return;
    char buf[4096];
    int n = getdents64(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return;
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        if (d->d_name[0] != '\0' && strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0) {
            char path[512];
            int plen = 0;
            const char *p = dir;
            while (*p && plen < 511) path[plen++] = *p++;
            if (plen > 0 && path[plen-1] != '/') path[plen++] = '/';
            p = d->d_name;
            while (*p && plen < 511) path[plen++] = *p++;
            path[plen] = '\0';
            if (strstr(d->d_name, pattern)) {
                printf("%s\n", path);
                found++;
            }
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                search_dir(path, pattern);
            }
        }
        pos += d->d_reclen;
    }
}

int main(int argc, char *argv[]) {
    const char *dir = ".";
    const char *pattern = "";
    int i = 1;
    while (i < argc) {
        if (i + 1 < argc && strcmp(argv[i], "-name") == 0) { pattern = argv[i + 1]; i += 2; }
        else { dir = argv[i]; i++; }
    }
    search_dir(dir, pattern);
    if (found == 0 && pattern[0] != '\0') { /* no match, not an error */ }
    return 0;
}
