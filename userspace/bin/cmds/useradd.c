/* useradd.c — Add user */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Check if username exists */
static int user_exists(const char *username) {
    int fd = open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) return 0;

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    char *line = buf;
    while (*line) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next == '\n') *next++ = 0;
        else next = line + strlen(line);

        /* Check first field (username) */
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            if (strcmp(line, username) == 0) {
                return 1;
            }
        }

        line = next;
    }
    return 0;
}

/* Find highest UID */
static int find_max_uid(void) {
    int max_uid = 0;
    int fd = open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) return 1000;

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1000;
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

        if (fc >= 3) {
            int uid = atoi(fields[2]);
            if (uid > max_uid) max_uid = uid;
        }

        line = next;
    }

    return max_uid;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: useradd <username>\n");
        return 1;
    }

    const char *username = argv[1];

    if (user_exists(username)) {
        printf("useradd: user '%s' already exists\n", username);
        return 1;
    }

    int new_uid = find_max_uid() + 1;
    int new_gid = new_uid;

    /* Append to /etc/passwd */
    int fd = open("/etc/passwd", O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        /* File might not exist yet */
        fd = open("/etc/passwd", O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            printf("useradd: cannot open /etc/passwd\n");
            return 1;
        }
    }

    char line[512];
    int len = snprintf(line, sizeof(line), "%s::%d:%d::/home/%s:/bin/sh\n",
        username, new_uid, new_gid, username);

    if (write(fd, line, len) != len) {
        printf("useradd: write error\n");
        close(fd);
        return 1;
    }

    close(fd);

    /* Create home directory */
    char home[256];
    snprintf(home, sizeof(home), "/home/%s", username);
    mkdir(home, 0755);

    printf("useradd: added user '%s' (uid=%d, gid=%d)\n", username, new_uid, new_gid);
    return 0;
}
