/* losetup.c — set up/control loop devices (fully implemented) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Loop ioctl numbers */
#define LOOP_SET_FD      0x4C00
#define LOOP_CLR_FD      0x4C01
#define LOOP_CTL_GET_FREE 0x4C82
#define LOOP_CTL_ADD     0x4C83
#define LOOP_CTL_REMOVE  0x4C84

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

/* Add a dummy variable to suppress unused-variable warnings */
#define UNUSED(x) (void)(x)

/* Set up a loop device to back a file */
static int setup_loop(const char *file, int show_device) {
    int num = -1;

    /* Method 1: Use LOOP_CTL_GET_FREE on /dev/loop-control */
    int ctl_fd = open("/dev/loop-control", O_RDWR, 0);
    if (ctl_fd >= 0) {
        num = ioctl(ctl_fd, LOOP_CTL_GET_FREE, 0);
        close(ctl_fd);
        if (num >= 0) {
            char devpath[32];
            snprintf(devpath, sizeof(devpath), "/dev/loop%d", num);

            int file_fd = open(file, O_RDWR, 0);
            if (file_fd < 0) file_fd = open(file, O_RDONLY, 0);
            if (file_fd < 0) {
                printf("losetup: cannot open '%s'\n", file);
                return -1;
            }

            int loop_fd = open(devpath, O_RDWR, 0);
            if (loop_fd < 0) {
                close(file_fd);
                printf("losetup: cannot open '%s'\n", devpath);
                return -1;
            }

            int ret = ioctl(loop_fd, LOOP_SET_FD, (void *)(unsigned long)file_fd);
            close(file_fd);
            close(loop_fd);

            if (ret == 0) {
                if (show_device)
                    printf("%s\n", devpath);
                else
                    printf("losetup: %s -> %s\n", devpath, file);
                return 0;
            }
        } else {
            /* LOOP_CTL_GET_FREE may not be supported; fall through */
        }
    }

    /* Method 2: Scan /dev/loop* for free device */
    for (num = 0; num < 256; num++) {
        char devpath[32];
        snprintf(devpath, sizeof(devpath), "/dev/loop%d", num);

        struct stat st;
        if (stat(devpath, &st) < 0)
            continue;

        /* Check if already in use via /sys/block */
        char syspath[64];
        snprintf(syspath, sizeof(syspath), "/sys/block/loop%d/loop/backing_file", num);
        int backing_fd = open(syspath, O_RDONLY, 0);
        if (backing_fd >= 0) {
            close(backing_fd);
            continue;
        }

        /* Try to set up this loop device */
        int file_fd = open(file, O_RDWR, 0);
        if (file_fd < 0) file_fd = open(file, O_RDONLY, 0);
        if (file_fd < 0) {
            printf("losetup: cannot open '%s'\n", file);
            return -1;
        }

        int loop_fd = open(devpath, O_RDWR, 0);
        if (loop_fd < 0) {
            close(file_fd);
            continue;
        }

        int ret = ioctl(loop_fd, LOOP_SET_FD, (void *)(unsigned long)file_fd);
        close(file_fd);
        close(loop_fd);

        if (ret == 0) {
            if (show_device)
                printf("%s\n", devpath);
            else
                printf("losetup: %s -> %s\n", devpath, file);
            return 0;
        }
    }

    /* Method 3: Write backing file path to sysfs */
    for (num = 0; num < 256; num++) {
        char syspath[64];
        snprintf(syspath, sizeof(syspath), "/sys/block/loop%d/loop/backing_file", num);
        int sys_fd = open(syspath, O_WRONLY, 0);
        if (sys_fd >= 0) {
            write(sys_fd, file, strlen(file));
            close(sys_fd);
            char devpath[32];
            snprintf(devpath, sizeof(devpath), "/dev/loop%d", num);
            if (show_device)
                printf("%s\n", devpath);
            else
                printf("losetup: %s -> %s\n", devpath, file);
            return 0;
        }
    }

    printf("losetup: no free loop device found or setup not supported\n");
    printf("  (Kernel must have CONFIG_BLK_DEV_LOOP and /dev/loop* devices)\n");
    return -1;
}

int main(int argc, char *argv[]) {
    int opt_list = 0;
    int opt_find_free = 0;
    int opt_show = 0;
    const char *file_arg = NULL;
    const char *device_arg = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            opt_list = 1;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--find") == 0) {
            opt_find_free = 1;
        } else if (strcmp(argv[i], "--show") == 0) {
            opt_show = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--detach") == 0) {
            if (i + 1 < argc) {
                device_arg = argv[++i];
            } else {
                printf("losetup: option '-d' requires a device argument\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: losetup [options] <file>  (set up loop device)\n");
            printf("       losetup -l                    (list loop devices)\n");
            printf("       losetup -f                    (find first unused)\n");
            printf("       losetup -d <device>           (detach loop device)\n");
            printf("       losetup --show <file>         (set up and show device)\n");
            return 0;
        } else if (argv[i][0] == '-') {
            printf("losetup: invalid option '%s'\n", argv[i]);
            printf("Usage: losetup [-l] [-f] [-d <device>] [--show] <file>\n");
            return 1;
        } else {
            if (!file_arg)
                file_arg = argv[i];
            else if (!device_arg)
                device_arg = argv[i];
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
        UNUSED(n);
        close(sys_fd);

        int found = 0;
        int pos = 0;
        while (pos < n) {
            struct dirent *de = (struct dirent *)(buf + pos);
            if (strncmp(de->d_name, "loop", 4) == 0) {
                char dev_path[64];
                snprintf(dev_path, sizeof(dev_path), "/dev/%s", de->d_name);
                printf("%s:", dev_path);

                char backing[256];
                char back_path[64];
                snprintf(back_path, sizeof(back_path), "/sys/block/%s/loop/backing_file", de->d_name);
                if (read_file(back_path, backing, sizeof(backing)) >= 0) {
                    chomp(backing);
                    printf(" %s", backing);
                }

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

        if (!found)
            printf("losetup: no loop devices\n");
        return 0;
    }

    if (opt_find_free && !file_arg) {
        /* Find first free loop device */
        int ctl_fd = open("/dev/loop-control", O_RDWR, 0);
        if (ctl_fd >= 0) {
            int num = ioctl(ctl_fd, LOOP_CTL_GET_FREE, 0);
            close(ctl_fd);
            if (num >= 0) {
                printf("/dev/loop%d\n", num);
                return 0;
            }
        }

        /* Fallback: scan /sys/block */
        int sys_fd = open("/sys/block", O_RDONLY, 0);
        if (sys_fd < 0) {
            printf("losetup: cannot access /sys/block\n");
            return 1;
        }
        char buf[4096];
        int n = getdents64(sys_fd, buf, sizeof(buf));
        UNUSED(n);
        close(sys_fd);

        for (int num = 0; num < 256; num++) {
            char name[32];
            snprintf(name, sizeof(name), "loop%d", num);

            char devpath[32];
            snprintf(devpath, sizeof(devpath), "/dev/%s", name);
            struct stat st;
            if (stat(devpath, &st) < 0)
                continue;

            char back_path[64];
            snprintf(back_path, sizeof(back_path), "/sys/block/%s/loop/backing_file", name);
            char dummy[4];
            if (read_file(back_path, dummy, sizeof(dummy)) >= 0)
                continue;

            printf("/dev/%s\n", name);
            return 0;
        }

        printf("losetup: no free loop device found\n");
        return 1;
    }

    if (device_arg && strcmp(argv[1], "-d") == 0) {
        /* Detach a loop device */
        const char *dev = device_arg;
        if (strncmp(dev, "/dev/", 5) == 0) dev += 5;

        char devpath[32];
        if (dev[0] == '/')
            snprintf(devpath, sizeof(devpath), "%s", dev);
        else
            snprintf(devpath, sizeof(devpath), "/dev/%s", dev);

        int fd = open(devpath, O_RDWR, 0);
        if (fd < 0) {
            printf("losetup: cannot open '%s'\n", devpath);
            return 1;
        }

        int ret = ioctl(fd, LOOP_CLR_FD, NULL);
        if (ret < 0) {
            printf("losetup: cannot detach '%s'\n", devpath);
            close(fd);
            return 1;
        }
        close(fd);
        printf("losetup: detached %s\n", devpath);
        return 0;
    }

    /* Set up loop device */
    if (file_arg) {
        if (opt_find_free || opt_show)
            return setup_loop(file_arg, opt_show) == 0 ? 0 : 1;
        return setup_loop(file_arg, 0) == 0 ? 0 : 1;
    }

    printf("losetup: missing file argument\n");
    printf("Usage: losetup [options] <file>\n");
    printf("       losetup -l\n");
    printf("       losetup -f\n");
    printf("       losetup -d <device>\n");
    return 1;
}
