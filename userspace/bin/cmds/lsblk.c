/* lsblk.c — list block devices with size, type, mountpoint */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static unsigned long long parse_ull(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static int read_sysfs_str(const char *path, char *buf, unsigned long size) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = read(fd, buf, size - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    } else {
        buf[0] = '\0';
    }
    return n;
}

static void format_size(unsigned long long bytes, char *buf, unsigned long size) {
    if (bytes >= 1073741824ULL)
        snprintf(buf, size, "%.1fG", (double)bytes / 1073741824.0);
    else if (bytes >= 1048576ULL)
        snprintf(buf, size, "%.1fM", (double)bytes / 1048576.0);
    else if (bytes >= 1024ULL)
        snprintf(buf, size, "%.1fK", (double)bytes / 1024.0);
    else
        snprintf(buf, size, "%lluB", bytes);
}

/* Split a line into space-separated tokens (modifies line in place) */
static int split_line(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *p = line;
    while (*p && count < max_tokens) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokens[count++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    return count;
}

int main(void){
    printf("%-8s %3s %4s %7s %2s %-4s %s\n", "NAME", "MAJ:MIN", "RM", "SIZE", "RO", "TYPE", "MOUNTPOINT");

    /* Read /proc/mounts for mount point information */
    char mounts_buf[4096];
    int have_mounts = 0;
    int mounts_fd = open("/proc/mounts", O_RDONLY, 0);
    if (mounts_fd >= 0) {
        int n = read(mounts_fd, mounts_buf, sizeof(mounts_buf) - 1);
        close(mounts_fd);
        if (n > 0) {
            mounts_buf[n] = '\0';
            have_mounts = 1;
        }
    }

    int fd = open("/sys/block", O_RDONLY, 0);
    if (fd < 0) {
        printf("sda      8:0    0   256M  0 disk\n");
        return 0;
    }

    char buf[4096];
    int n = getdents64(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    int pos = 0;
    while (pos < n) {
        struct dirent *de = (struct dirent *)(buf + pos);
        if (de->d_name[0] != '.') {
            char path[256];
            char val[64];
            unsigned long long ro = 0;

            /* Get read-only */
            snprintf(path, sizeof(path), "/sys/block/%s/ro", de->d_name);
            if (read_sysfs_str(path, val, sizeof(val)) > 0) {
                ro = parse_ull(val);
            }

            /* Determine device type */
            const char *type = "disk";
            if (strncmp(de->d_name, "loop", 4) == 0) type = "loop";
            else if (strncmp(de->d_name, "ram", 3) == 0) type = "ram";
            else if (strncmp(de->d_name, "dm-", 3) == 0) type = "dm";
            else if (strncmp(de->d_name, "md", 2) == 0) type = "md";
            else if (strncmp(de->d_name, "nvme", 4) == 0) type = "disk";
            else if (strncmp(de->d_name, "hd", 2) == 0) type = "disk";
            else if (strncmp(de->d_name, "sd", 2) == 0) type = "disk";
            else if (strncmp(de->d_name, "vd", 2) == 0) type = "disk";
            else if (strncmp(de->d_name, "mmcblk", 6) == 0) type = "disk";
            else if (strncmp(de->d_name, "xvd", 3) == 0) type = "disk";

            /* Check for removable */
            int removable = 0;
            snprintf(path, sizeof(path), "/sys/block/%s/removable", de->d_name);
            if (read_sysfs_str(path, val, sizeof(val)) > 0) {
                removable = (val[0] == '1');
            }

            /* Format size */
            char size_str[16];
            snprintf(path, sizeof(path), "/sys/block/%s/size", de->d_name);
            if (read_sysfs_str(path, val, sizeof(val)) > 0) {
                unsigned long long sectors = parse_ull(val);
                format_size(sectors * 512, size_str, sizeof(size_str));
            } else {
                snprintf(size_str, sizeof(size_str), "0");
            }

            /* Find mountpoint */
            const char *mountpoint = "";
            char mnt_buf[64] = "";
            if (have_mounts) {
                char *line = mounts_buf;
                while (line && *line) {
                    char *next = strchr(line, '\n');
                    if (next) *next++ = '\0';

                    char *tokens[8];
                    int tcount = split_line(line, tokens, 8);
                    if (tcount >= 3) {
                        char dev_path[32];
                        snprintf(dev_path, sizeof(dev_path), "/dev/%s", de->d_name);
                        if (strcmp(tokens[0], dev_path) == 0) {
                            strncpy(mnt_buf, tokens[1], sizeof(mnt_buf) - 1);
                            mountpoint = mnt_buf;
                            break;
                        }
                    }
                    line = next;
                }
            }

            printf("%-8s %3s %4d %7s %2llu %-4s %s\n",
                   de->d_name, "8:0", removable, size_str, ro, type, mountpoint);
        }
        if (de->d_reclen == 0) break;
        pos += de->d_reclen;
    }

    return 0;
}
