/* ebtables.c — Ethernet bridge table management */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void read_sysfs_str(const char *path, char *buf, unsigned long size) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { buf[0] = '\0'; return; }
    int n = read(fd, buf, size - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    } else {
        buf[0] = '\0';
    }
}

static void show_bridge_info(const char *ifname) {
    char path[128];
    char val[256];

    printf("Bridge: %s\n", ifname);

    snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/bridge_id", ifname);
    read_sysfs_str(path, val, sizeof(val));
    if (val[0]) printf("  Bridge ID:      %s\n", val);

    snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/stp_state", ifname);
    read_sysfs_str(path, val, sizeof(val));
    if (val[0]) printf("  STP Enabled:    %s\n", val[0] == '1' ? "yes" : val[0] == '0' ? "no" : val);

    snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/forward_delay", ifname);
    read_sysfs_str(path, val, sizeof(val));
    if (val[0]) printf("  Forward delay:  %s\n", val);

    snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/multicast_router", ifname);
    read_sysfs_str(path, val, sizeof(val));
    if (val[0]) printf("  Multicast:      %s\n", val);

    snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifname);
    read_sysfs_str(path, val, sizeof(val));
    if (val[0]) printf("  Address:        %s\n", val);

    /* Show bridge ports */
    snprintf(path, sizeof(path), "/sys/class/net/%s/brif", ifname);
    int brfd = open(path, O_RDONLY, 0);
    if (brfd >= 0) {
        char dents[1024];
        int n = getdents64(brfd, dents, sizeof(dents));
        close(brfd);
        if (n > 0) {
            printf("  Ports:         ");
            unsigned long pos = 0;
            while (pos < (unsigned long)n) {
                struct dirent *de = (struct dirent *)(dents + pos);
                if (de->d_name[0] != '.') {
                    printf(" %s", de->d_name);
                }
                if (de->d_reclen == 0) break;
                pos += de->d_reclen;
            }
            printf("\n");
        }
    } else {
        printf("  Ports:         (none)\n");
    }

    printf("\n");
}

int main(int argc, char *argv[]) {
    int list_mode = 0;
    const char *target_bridge = NULL;

    if (argc > 1) {
        if (strcmp(argv[1], "-L") == 0 || strcmp(argv[1], "--list") == 0) {
            list_mode = 1;
        } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: ebtables -L|--list [bridge]  (list bridge rules)\n");
            return 0;
        } else if (argv[1][0] != '-') {
            target_bridge = argv[1];
            list_mode = 1;
        } else {
            printf("ebtables: unknown option '%s'\n", argv[1]);
            return 1;
        }
    } else {
        list_mode = 1;
    }

    if (list_mode) {
        if (target_bridge) {
            show_bridge_info(target_bridge);
        } else {
            /* List all bridges */
            int fd = open("/sys/class/net", O_RDONLY, 0);
            if (fd < 0) {
                printf("ebtables: cannot access /sys/class/net\n");
                return 1;
            }
            char dents[4096];
            int n = getdents64(fd, dents, sizeof(dents));
            close(fd);
            if (n <= 0) {
                printf("ebtables: no network interfaces\n");
                return 1;
            }
            int found = 0;
            unsigned long pos = 0;
            while (pos < (unsigned long)n) {
                struct dirent *de = (struct dirent *)(dents + pos);
                if (de->d_name[0] != '.') {
                    char path[128];
                    snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/bridge_id", de->d_name);
                    int bfd = open(path, O_RDONLY, 0);
                    if (bfd >= 0) {
                        close(bfd);
                        show_bridge_info(de->d_name);
                        found = 1;
                    }
                }
                if (de->d_reclen == 0) break;
                pos += de->d_reclen;
            }
            if (!found) {
                printf("ebtables: no bridge interfaces found\n");
            }
        }
        return 0;
    }

    return 0;
}
