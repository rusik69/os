/* tc.c — Traffic control: display network QoS/scheduling info from kernel */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void show_qdiscs_for_iface(const char *ifname) {
    char path[128];
    char buf[256];

    printf("qdisc %s: ", ifname);

    /* Try /sys/class/net/<iface>/qdisc or tx_queue_len as hints */
    snprintf(path, sizeof(path), "/sys/class/net/%s/tx_queue_len", ifname);
    int fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf("tx_queue_len %s", buf);
        }
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", ifname);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf(" mtu %s", buf);
        }
    }

    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", ifname);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf(" state %s", buf);
        }
    }

    printf("\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: tc qdisc show [dev <iface>]\n");
        printf("       tc class show dev <iface>\n");
        printf("\n");
        printf("Kernel traffic control information:\n");

        /* Try to read /proc/net/tc if it exists */
        int fd = open("/proc/net/tc", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[4096];
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                write(1, buf, n);
            close(fd);
            return 0;
        }

        /* List all interfaces with qdisc info */
        fd = open("/sys/class/net", O_RDONLY, 0);
        if (fd < 0) {
            printf("tc: no network interfaces available\n");
            return 0;
        }
        char dents[4096];
        int n = getdents64(fd, dents, sizeof(dents));
        close(fd);
        if (n <= 0) return 0;

        unsigned long pos = 0;
        while (pos < (unsigned long)n) {
            struct dirent *de = (struct dirent *)(dents + pos);
            if (de->d_name[0] != '.') {
                show_qdiscs_for_iface(de->d_name);
            }
            if (de->d_reclen == 0) break;
            pos += de->d_reclen;
        }
        return 0;
    }

    if (strcmp(argv[1], "qdisc") == 0) {
        const char *dev = NULL;
        if (argc > 2 && strcmp(argv[2], "show") == 0) {
            if (argc > 3 && strcmp(argv[3], "dev") == 0 && argc > 4) {
                dev = argv[4];
            }
            if (dev) {
                show_qdiscs_for_iface(dev);
            } else {
                /* Show all */
                int fd = open("/sys/class/net", O_RDONLY, 0);
                if (fd < 0) return 0;
                char dents[4096];
                int n = getdents64(fd, dents, sizeof(dents));
                close(fd);
                if (n <= 0) return 0;
                unsigned long pos = 0;
                while (pos < (unsigned long)n) {
                    struct dirent *de = (struct dirent *)(dents + pos);
                    if (de->d_name[0] != '.') {
                        show_qdiscs_for_iface(de->d_name);
                    }
                    if (de->d_reclen == 0) break;
                    pos += de->d_reclen;
                }
            }
            return 0;
        }
    }

    printf("tc: only 'qdisc show' is supported\n");
    return 0;
}
