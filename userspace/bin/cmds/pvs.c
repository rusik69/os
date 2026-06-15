/* pvs.c — list physical volumes (block devices from /sys/block) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define S_IFMT   0170000
#define S_IFBLK  0060000
#define S_IFCHR  0020000
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)  (((m) & S_IFMT) == 0040000)

static void human_size(unsigned long long bytes, char *buf, int bufsz) {
    const char *units[] = {"B", "K", "M", "G", "T", "P"};
    int ui = 0;
    unsigned long long val = bytes;
    while (val >= 1024 && ui < 5) {
        val /= 1024;
        ui++;
    }
    int n = 0;
    if (ui == 0) {
        n = snprintf(buf, bufsz, "%lluB", bytes);
    } else {
        /* Show with one decimal if remainder is significant */
        unsigned long long remainder = bytes;
        unsigned long long divisor = 1;
        for (int i = 0; i < ui; i++) divisor *= 1024;
        remainder = bytes % divisor;
        unsigned long long dec = (remainder * 10) / divisor;
        if (dec > 0)
            n = snprintf(buf, bufsz, "%llu.%llu%s", val, dec, units[ui]);
        else
            n = snprintf(buf, bufsz, "%llu%s", val, units[ui]);
    }
    (void)n;
    buf[bufsz - 1] = 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int fd = open("/sys/block", O_RDONLY, 0);
    if (fd < 0) {
        /* Fallback: try /dev */
        fd = open("/dev", O_RDONLY, 0);
        if (fd < 0) {
            printf("pvs: cannot open /sys/block or /dev\n");
            return 1;
        }
    }

    char buf[4096];
    int n = getdents64(fd, buf, 4096);
    close(fd);

    if (n <= 0) {
        printf("pvs: no entries found\n");
        return 0;
    }

    printf("%-20s %12s  %s\n", "NAME", "SIZE", "TYPE");
    printf("%-20s %12s  %s\n", "----", "----", "----");

    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        pos += d->d_reclen;

        /* Skip . and .. */
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;

        /* Build full path for stat */
        char fullpath[256];
        int plen = strlen("/sys/block/");
        if (plen + strlen(d->d_name) + 1 > 256) continue;
        memcpy(fullpath, "/sys/block/", plen);
        memcpy(fullpath + plen, d->d_name, strlen(d->d_name) + 1);

        struct stat st;
        if (stat(fullpath, &st) < 0) {
            /* Try /dev/ prefix for fallback */
            snprintf(fullpath, 256, "/dev/%s", d->d_name);
            if (stat(fullpath, &st) < 0)
                continue;
        }

        const char *type = "unknown";
        if (S_ISBLK(st.st_mode)) type = "block";
        else if (S_ISCHR(st.st_mode)) type = "char";
        else if (S_ISDIR(st.st_mode)) type = "dir";

        char sizebuf[32];
        human_size(st.st_size, sizebuf, sizeof(sizebuf));

        /* Try to read size from /sys/block/<name>/size for block devices */
        if (S_ISBLK(st.st_mode)) {
            char sizepath[256];
            snprintf(sizepath, 256, "/sys/block/%s/size", d->d_name);
            int sfd = open(sizepath, O_RDONLY, 0);
            if (sfd >= 0) {
                char sizestr[32];
                int r = read(sfd, sizestr, 31);
                close(sfd);
                if (r > 0) {
                    sizestr[r] = 0;
                    /* Remove trailing newline */
                    char *nl = strchr(sizestr, '\n');
                    if (nl) *nl = 0;
                    unsigned long long sectors = 0;
                    char *p = sizestr;
                    while (*p >= '0' && *p <= '9') {
                        sectors = sectors * 10 + (*p - '0');
                        p++;
                    }
                    unsigned long long bytes = sectors * 512;
                    human_size(bytes, sizebuf, sizeof(sizebuf));
                }
            }
        }

        printf("%-20s %12s  %s\n", d->d_name, sizebuf, type);
    }

    return 0;
}
