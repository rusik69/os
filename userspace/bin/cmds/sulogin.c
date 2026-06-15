/* sulogin.c — Single-user login (emergency root login) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static int find_user(const char *username, char *passwd) {
    int fd = open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    char *line = buf;
    while (*line) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next == '\n') *next++ = 0;
        else next = line + strlen(line);

        char *fields[7];
        int fc = 0;
        fields[fc++] = line;
        for (char *p = line; *p && fc < 7; p++) {
            if (*p == ':') {
                *p = 0;
                fields[fc++] = p + 1;
            }
        }

        if (fc >= 7 && strcmp(fields[0], username) == 0) {
            if (passwd) strncpy(passwd, fields[1], 128);
            return 0;
        }

        line = next;
    }

    return -1;
}

static void read_pass(char *buf, int size) {
    int pos = 0;
    char c;
    while (pos < size - 1) {
        if (read(0, &c, 1) <= 0) break;
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) {
            if (pos > 0) pos--;
            continue;
        }
        buf[pos++] = c;
    }
    buf[pos] = 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Single-user login (emergency access)\n");

    char stored_passwd[128];
    if (find_user("root", stored_passwd) < 0) {
        printf("sulogin: root account not found, starting shell anyway\n");
        char *sh_argv[2] = { "/bin/sh", NULL };
        execve("/bin/sh", sh_argv, (char *const[]){ NULL });
        printf("sulogin: cannot start shell\n");
        return 1;
    }

    printf("Give root password for maintenance\n");
    printf("(or type Control-D to continue): ");
    char input_pass[128];
    read_pass(input_pass, sizeof(input_pass));

    /* If empty password input (just Enter), proceed without auth */
    if (input_pass[0] != 0 && strcmp(stored_passwd, input_pass) != 0) {
        printf("sulogin: incorrect password\n");
        return 1;
    }

    printf("Starting shell for root maintenance...\n");

    char *sh_argv[2] = { "/bin/sh", NULL };
    execve("/bin/sh", sh_argv, (char *const[]){ NULL });

    printf("sulogin: cannot start shell\n");
    return 1;
}
