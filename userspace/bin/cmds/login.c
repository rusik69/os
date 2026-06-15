/* login.c — User login */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Read a line of input from stdin (no echo for passwords) */
static void read_line(char *buf, int size, int echo) {
    int pos = 0;
    char c;
    while (pos < size - 1) {
        if (read(0, &c, 1) <= 0) break;
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                if (echo) write(1, "\b \b", 3);
            }
            continue;
        }
        buf[pos++] = c;
        if (echo) write(1, &c, 1);
    }
    buf[pos] = 0;
    if (echo) write(1, "\n", 1);
}

/* Find user in /etc/passwd. Returns 0 on success with fields filled. */
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

        /* Parse line: username:password:uid:gid:gecos:home:shell */
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

int main(int argc, char *argv[]) {
    const char *default_username = (argc > 1) ? argv[1] : 0;

    /* If stdin is not a tty, read username from stdin */
    char username[64];
    if (default_username) {
        strncpy(username, default_username, sizeof(username));
    } else {
        printf("login: ");
        read_line(username, sizeof(username), 1);
    }

    char stored_passwd[128];
    int uid, gid;
    char home[256], shell[256];

    if (find_user(username, stored_passwd, &uid, &gid, home, shell) < 0) {
        printf("login: unknown user '%s'\n", username);
        return 1;
    }

    /* Verify password */
    printf("Password: ");
    char input_pass[128];
    read_line(input_pass, sizeof(input_pass), 0); /* no echo */

    if (strcmp(stored_passwd, input_pass) != 0) {
        printf("login: incorrect password\n");
        return 1;
    }

    printf("login: starting shell for %s\n", username);

    /* Change to home directory */
    if (home[0])
        chdir(home);

    /* Start shell */
    char *sh_argv[2] = { "/bin/sh", NULL };
    execve("/bin/sh", sh_argv, (char *const[]){ NULL });

    printf("login: cannot start shell\n");
    return 1;
}
