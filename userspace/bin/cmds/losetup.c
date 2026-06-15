/* losetup.c — set up/control loop devices */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Simple unsigned long long parse */
static unsigned long long parse_ull(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* Read a file fully into static buffer */
static int read_file(const char *path, char *buf, unsigned long size) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

/* Strip trailing newline */
static void chomp(char *s) {
    unsigned long len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = 0;
}

int main(int argc, char *argv[]) {
    int opt_list = 0;
    int opt_find_free = 0;
    const char *file_arg = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            opt_list = 1;
        } else if (strcmp(argv[i], "-f") == 0) {
            opt_find_free = 1;
        } else if (strcmp(argv[i], "--show") == 0) {
            /* --show is silently accepted */
        } else if (argv[i][0] == '-') {
            printf("losetup: invalid option '%s'\n", argv[i]);
            printf("Usage: losetup [-f] [--show] <file>\n");
            printf("       losetup -l\n");
            return 1;
        } else {
            if (!file_arg)
                file_arg = argv[i];
            /* else: device arg (not used in this impl) */
        }
    }

    if (opt_list) {
        /* List loop devices */
        int sys_fd = open("/sys/block", O_RDONLY, 0);
        if (sys_fd < 0) {
            printf("losetup: cannot access /sys/block\n");
            return 1;
        }
        char buf[4096];
        int n = getdents64(sys_fd, buf, sizeof(buf));
        close(sys_fd);
        if (n < 0) return 1;

        int found = 0;
        int pos = 0;
        while (pos < n) {
            struct dirent *de = (struct dirent *)(buf + pos);
            if (strncmp(de->d_name, "loop", 4) == 0) {
                char dev_path[64];
                snprintf(dev_path, sizeof(dev_path), "/dev/%s", de->d_name);
                printf("%s:", dev_path);

                /* Try to read backing file */
                char backing[256];
                char back_path[64];
                snprintf(back_path, sizeof(back_path), "/sys/block/%s/loop/backing_file", de->d_name);
                if (read_file(back_path, backing, sizeof(backing)) >= 0) {
                    chomp(backing);
                    printf(" %s", backing);
                }

                /* Try to read size */
                char size_path[64];
                snprintf(size_path, sizeof(size_path), "/sys/block/%s/size", de->d_name);
                char size_str[32];
                if (read_file(size_path, size_str, sizeof(size_str)) >= 0) {
                    chomp(size_str);
                    unsigned long long sectors = parse_ull(size_str);
                    printf(" [%llu bytes]", sectors * 512);
                }

                printf("\n");
                found = 1;
            }
            if (de->d_reclen == 0) break;
            pos += de->d_reclen;
        }

        if (!found) {
            printf("losetup: no loop devices\n");
        }
        return 0;
    }

    if (opt_find_free) {
        /* Find first free loop device */
        int sys_fd = open("/sys/block", O_RDONLY, 0);
        if (sys_fd < 0) {
            printf("losetup: cannot access /sys/block\n");
            return 1;
        }
        char buf[4096];
        int n = getdents64(sys_fd, buf, sizeof(buf));
        close(sys_fd);

        int loop_num = 0;
        int found = 0;
        /* Loop numbers typically 0-7 */
        for (int num = 0; num < 256; num++) {
            char name[32];
            snprintf(name, sizeof(name), "loop%d", num);
            int occupied = 0;
            int pos = 0;
            while (pos < n) {
                struct dirent *de = (struct dirent *)(buf + pos);
                if (strcmp(de->d_name, name) == 0) {
                    occupied = 1;
                    /* Check if it has a backing file (i.e., it's in use) */
                    char back_path[64];
                    snprintf(back_path, sizeof(back_path), "/sys/block/%s/loop/backing_file", name);
                    char dummy[4];
                    if (read_file(back_path, dummy, sizeof(dummy)) >= 0) {
                        /* In use */
                    } else {
                        found = 1;
                        loop_num = num;
                        goto found_free;
                    }
                    break;
                }
                if (de->d_reclen == 0) break;
                pos += de->d_reclen;
            }
            if (!occupied) {
                found = 1;
                loop_num = num;
                goto found_free;
            }
        }
        found_free:;
        if (found) {
            printf("/dev/loop%d\n", loop_num);
        } else {
            printf("losetup: no free loop device\n");
            return 1;
        }
    }

    /* If file_arg given, set up a loop device */
    if (file_arg && !opt_find_free) {
        printf("losetup: would set up '%s' on loop device\n", file_arg);
        printf("losetup: loop device setup not fully implemented in this build\n");
        return 1;
    }

    return 0;
}
