/* mdev.c — device listing from /dev */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define S_IFMT   0170000
#define S_IFBLK  0060000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

static const char *type_str(unsigned int mode) {
    if (S_ISBLK(mode)) return "block";
    if (S_ISCHR(mode)) return "char";
    if (S_ISDIR(mode)) return "dir";
    if (S_ISREG(mode)) return "file";
    return "other";
}

int main(int argc, char *argv[]) {
    int scan_mode = 0;

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        scan_mode = 1;
    }

    int fd = open("/dev", O_RDONLY, 0);
    if (fd < 0) {
        printf("mdev: cannot open /dev\n");
        return 1;
    }

    char buf[8192];
    int n = getdents64(fd, buf, sizeof(buf));
    close(fd);

    if (n <= 0) {
        printf("mdev: no entries\n");
        return 0;
    }

    printf("Device scan%s:\n", scan_mode ? " (full scan)" : "");
    printf("%-24s %-8s %s\n", "NAME", "TYPE", "MAJOR:MINOR");
    printf("%-24s %-8s %s\n", "----", "----", "---------");

    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        pos += d->d_reclen;

        /* Skip . and .. */
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;

        /* Stat the entry */
        char fullpath[256];
        int plen = strlen("/dev/");
        if (plen + strlen(d->d_name) + 1 > 256) continue;
        memcpy(fullpath, "/dev/", plen);
        memcpy(fullpath + plen, d->d_name, strlen(d->d_name) + 1);

        struct stat st;
        if (stat(fullpath, &st) < 0) {
            /* Print at least the name */
            printf("%-24s %-8s %s\n", d->d_name, "?", "?:?");
            continue;
        }

        /* Extract major:minor from st_rdev for device files */
        char devbuf[16] = "?:?";
        if (S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode)) {
            unsigned long long rdev = st.st_rdev;
            unsigned int major = (unsigned int)((rdev >> 32) & 0xFFFF);
            unsigned int minor = (unsigned int)(rdev & 0xFFFF);
            snprintf(devbuf, sizeof(devbuf), "%u:%u", major, minor);
        }

        printf("%-24s %-8s %s\n", d->d_name, type_str(st.st_mode), devbuf);
    }

    return 0;
}
