/* userdel.c — Delete user */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: userdel <username>\n");
        return 1;
    }

    const char *username = argv[1];

    /* Prevent deleting root */
    if (strcmp(username, "root") == 0) {
        printf("userdel: cannot delete root account\n");
        return 1;
    }

    /* Read /etc/passwd, remove the user's line, write back */
    int fd = open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) {
        printf("userdel: cannot open /etc/passwd\n");
        return 1;
    }

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        printf("userdel: /etc/passwd is empty\n");
        return 1;
    }
    buf[n] = 0;

    /* Rebuild file without the target user */
    char newfile[8192];
    int pos = 0;
    int found = 0;

    char *line = buf;
    while (*line) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next == '\n') *next++ = 0;
        else next = line + strlen(line);

        /* Check first field */
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            if (strcmp(line, username) == 0) {
                found = 1;
                line = next;
                continue;
            }
            /* Restore colon */
            *colon = ':';
        }

        /* Write this line */
        int linelen = strlen(line);
        if (pos + linelen + 1 < (int)sizeof(newfile)) {
            memcpy(newfile + pos, line, linelen);
            pos += linelen;
            newfile[pos++] = '\n';
        }

        line = next;
    }

    newfile[pos] = 0;

    if (!found) {
        printf("userdel: user '%s' not found\n", username);
        return 1;
    }

    /* Write back */
    fd = open("/etc/passwd", O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("userdel: cannot write /etc/passwd\n");
        return 1;
    }

    int w = write(fd, newfile, pos);
    close(fd);

    if (w != pos) {
        printf("userdel: write error\n");
        return 1;
    }

    printf("userdel: deleted user '%s'\n", username);
    return 0;
}
