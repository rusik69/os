/* passwd.c — change user password */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

/* Read a line from stdin, no echo (simplified) */
static int read_line(char *buf, int max) {
    int i = 0;
    char c;
    while (i < max - 1) {
        if (read(0, &c, 1) != 1) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

int main(void) {
    char oldpw[128], newpw[128], newpw2[128];
    int retry = 3;

    printf("Changing password for current user.\n");

    while (retry-- > 0) {
        printf("Current password: ");
        read_line(oldpw, sizeof(oldpw));
        printf("\n");

        printf("New password: ");
        read_line(newpw, sizeof(newpw));
        printf("\n");

        printf("Retype new password: ");
        read_line(newpw2, sizeof(newpw2));
        printf("\n");

        if (strcmp(newpw, newpw2) != 0) {
            printf("passwd: passwords do not match\n");
            if (retry > 0) printf("Try again.\n");
            continue;
        }
        if (strlen(newpw) == 0) {
            printf("passwd: password cannot be empty\n");
            continue;
        }
        break;
    }

    if (strcmp(newpw, newpw2) != 0 || strlen(newpw) == 0)
        return 1;

    /* Attempt kernel-side password change via /proc interface */
    int fd = open("/proc/passwd", O_WRONLY, 0);
    if (fd >= 0) {
        char cmd[384];
        int n = snprintf(cmd, sizeof(cmd), "%s:%s", oldpw, newpw);
        write(fd, cmd, n < (int)sizeof(cmd) ? n : (int)sizeof(cmd));
        close(fd);
        printf("passwd: password changed successfully\n");
        return 0;
    }

    printf("passwd: kernel password change interface not available\n");
    printf("passwd: use 'passwd <user>' from the kernel shell\n");
    return 1;
}
