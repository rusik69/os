/* passwd.c — Change user password */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Read a line without echo */
static void read_noecho(char *buf, int size) {
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

/* Find user and get current password */
static int get_user_passwd(const char *username, char *stored_passwd, int *uid) {
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
            if (stored_passwd) strncpy(stored_passwd, fields[1], 128);
            if (uid) *uid = atoi(fields[2]);
            return 0;
        }

        line = next;
    }

    return -1;
}

/* Update password for a user in /etc/passwd */
static int update_passwd(const char *username, const char *new_passwd) {
    int fd = open("/etc/passwd", O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    /* Build new file content */
    char newfile[8192];
    int pos = 0;

    char *line = buf;
    while (*line) {
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next == '\n') *next++ = 0;
        else next = line + strlen(line);

        /* Save original line length before modifying */
        int orig_len = next - line;
        int is_target = 0;

        /* Parse fields without permanent modification */
        char *fields[7];
        int fc = 0;
        fields[fc++] = line;
        char *p = line;
        int colons_found = 0;
        while (*p && fc < 7) {
            if (*p == ':') {
                colons_found++;
                if (colons_found <= 6) {
                    *p = 0;
                    fields[fc++] = p + 1;
                }
            }
            p++;
        }

        if (fc >= 7 && strcmp(fields[0], username) == 0) {
            is_target = 1;
            pos += snprintf(newfile + pos, sizeof(newfile) - pos,
                "%s:%s:%s:%s:%s:%s:%s\n",
                fields[0], new_passwd, fields[2], fields[3],
                fields[4], fields[5], fields[6]);
        }

        if (!is_target) {
            /* Write original line — restore first ':' that was nulled */
            int orig_i;
            for (orig_i = 0; orig_i < 6 && line[orig_i]; orig_i++)
                ;
            /* line[orig_i] was ':', now null */
            if (line[orig_i] == 0 && orig_i < orig_len) {
                line[orig_i] = ':';
            }
            if (pos + orig_len + 1 < (int)sizeof(newfile)) {
                memcpy(newfile + pos, line, orig_len);
                pos += orig_len;
                newfile[pos++] = '\n';
            }
        }

        line = next;
    }

    newfile[pos] = 0;

    /* Write back to /etc/passwd */
    fd = open("/etc/passwd", O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int w = write(fd, newfile, pos);
    close(fd);
    return (w == pos) ? 0 : -1;
}

int main(int argc, char *argv[]) {
    static char username_buf[64];
    const char *username = 0;

    if (argc >= 2) {
        username = argv[1];
    } else {
        /* If no arg, use current user */
        int uid = getuid();

        int fd = open("/etc/passwd", O_RDONLY, 0);
        if (fd >= 0) {
            char pbuf[4096];
            int n = read(fd, pbuf, sizeof(pbuf) - 1);
            close(fd);
            if (n > 0) {
                pbuf[n] = 0;
                char *line = pbuf;
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

                    if (fc >= 7 && atoi(fields[2]) == uid) {
                        strncpy(username_buf, fields[0], sizeof(username_buf));
                        username = username_buf;
                        break;
                    }

                    line = next;
                }
            }
        }

        if (!username) {
            printf("passwd: cannot determine username\n");
            return 1;
        }
    }

    char stored_passwd[128];
    int uid;
    if (get_user_passwd(username, stored_passwd, &uid) < 0) {
        printf("passwd: unknown user '%s'\n", username);
        return 1;
    }

    /* Verify current password (root can skip) */
    int cur_uid = getuid();
    if (cur_uid != 0) {
        printf("Current password: ");
        char input_pass[128];
        read_noecho(input_pass, sizeof(input_pass));
        write(1, "\n", 1);

        if (strcmp(stored_passwd, input_pass) != 0) {
            printf("passwd: incorrect password\n");
            return 1;
        }
    }

    printf("New password: ");
    char new_pass1[128];
    read_noecho(new_pass1, sizeof(new_pass1));
    write(1, "\n", 1);

    printf("Retype new password: ");
    char new_pass2[128];
    read_noecho(new_pass2, sizeof(new_pass2));
    write(1, "\n", 1);

    if (strcmp(new_pass1, new_pass2) != 0) {
        printf("passwd: passwords do not match\n");
        return 1;
    }

    if (new_pass1[0] == 0) {
        printf("passwd: password cannot be empty\n");
        return 1;
    }

    if (update_passwd(username, new_pass1) < 0) {
        printf("passwd: failed to update password\n");
        return 1;
    }

    printf("passwd: password updated for '%s'\n", username);
    return 0;
}
