/* ifplugd.c — Network interface plug detection daemon */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("ifplugd: checking network interface status...\n");

    /* Check if network is present via net_present() syscall */
    int present = net_present();
    if (present >= 0) {
        if (present) {
            printf("ifplugd: link is UP\n");
        } else {
            printf("ifplugd: link is DOWN\n");
        }
        return 0;
    }

    /* Fallback: check /sys/class/net/ for link status */
    int fd = open("/sys/class/net", O_RDONLY, 0);
    if (fd >= 0) {
        char dents[2048];
        int n = getdents64(fd, dents, sizeof(dents));
        close(fd);

        if (n > 0) {
            unsigned long pos = 0;
            int found = 0;
            while (pos < (unsigned long)n) {
                struct dirent *de = (struct dirent *)&dents[pos];
                if (de->d_name[0] != '.') {
                    char path[128];
                    char buf[32];
                    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", de->d_name);
                    int sfd = open(path, O_RDONLY, 0);
                    if (sfd >= 0) {
                        int r = read(sfd, buf, sizeof(buf) - 1);
                        close(sfd);
                        if (r > 0) {
                            buf[r] = '\0';
                            while (r > 0 && (buf[r-1] == '\n' || buf[r-1] == ' ')) buf[--r] = '\0';
                            printf("ifplugd: %s: %s\n", de->d_name, buf);
                            found = 1;
                        }
                    }
                }
                pos += de->d_reclen;
            }
            if (found) return 0;
        }
    }

    printf("ifplugd: no network interfaces detected\n");
    return 1;
}
