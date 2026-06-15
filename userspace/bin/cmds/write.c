/* write.c — Write to another user's terminal */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define W_OK 2

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: write <user> [tty]\n");
        return 1;
    }

    const char *username = argv[1];
    const char *given_tty = (argc > 2) ? argv[2] : "";

    /* Find the target user's tty */
    char tty_path[128];
    tty_path[0] = 0;

    if (given_tty[0]) {
        if (given_tty[0] == '/')
            strncpy(tty_path, given_tty, sizeof(tty_path));
        else
            snprintf(tty_path, sizeof(tty_path), "/dev/%s", given_tty);
    }

    if (tty_path[0] == 0) {
        snprintf(tty_path, sizeof(tty_path), "/dev/tty1");
    }

    /* Read message from stdin */
    char msg[4096];
    int msg_len = 0;
    int n;
    while ((n = read(0, msg + msg_len, sizeof(msg) - msg_len - 1)) > 0) {
        msg_len += n;
    }
    msg[msg_len] = 0;

    /* Write to the target terminal */
    int tty_fd = open(tty_path, O_WRONLY, 0);
    if (tty_fd < 0) {
        printf("write: cannot open '%s'\n", tty_path);
        return 1;
    }

    /* Find our username */
    char our_name[64] = "unknown";
    int our_uid = getuid();
    int pfd = open("/etc/passwd", O_RDONLY, 0);
    if (pfd >= 0) {
        char pbuf[4096];
        int pn = read(pfd, pbuf, sizeof(pbuf) - 1);
        close(pfd);
        if (pn > 0) {
            pbuf[pn] = 0;
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
                    if (*p == ':') { *p = 0; fields[fc++] = p + 1; }
                }
                if (fc >= 3 && atoi(fields[2]) == our_uid) {
                    strncpy(our_name, fields[0], sizeof(our_name));
                    break;
                }
                line = next;
            }
        }
    }

    /* Format the message */
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "\nMessage from %s on console:\n", our_name);
    write(tty_fd, header, hlen);
    write(tty_fd, msg, msg_len);
    write(tty_fd, "\nEOF\n", 5);

    close(tty_fd);

    printf("write: message sent to %s on %s\n", username, tty_path);
    return 0;
}
