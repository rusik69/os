/* su.c — Substitute user identity */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static int find_user(const char *username, char *passwd, int *uid, int *gid, char *home, char *shell) {
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
            if (uid) *uid = atoi(fields[2]);
            if (gid) *gid = atoi(fields[3]);
            if (home) strncpy(home, fields[5], 256);
            if (shell) strncpy(shell, fields[6], 256);
            return 0;
        }

        line = next;
    }

    return -1;
}

/* Read a line without echo */
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
    const char *target = (argc > 1) ? argv[1] : "root";

    char stored_passwd[128];
    int uid, gid;
    char home[256], shell[256];

    if (find_user(target, stored_passwd, &uid, &gid, home, shell) < 0) {
        printf("su: unknown user '%s'\n", target);
        return 1;
    }

    /* Verify password if not root */
    int cur_uid = getuid();
    if (cur_uid != 0) {
        printf("Password: ");
        char input_pass[128];
        read_pass(input_pass, sizeof(input_pass));
        if (strcmp(stored_passwd, input_pass) != 0) {
            printf("su: incorrect password\n");
            return 1;
        }
    }

    printf("su: switching to user '%s'\n", target);

    if (home[0])
        chdir(home);

    char *sh_argv[2] = { "/bin/sh", NULL };
    execve("/bin/sh", sh_argv, (char *const[]){ NULL });

    printf("su: cannot start shell\n");
    return 1;
}
