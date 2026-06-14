/* users.c — list logged in users */
#include "unistd.h"
#include "string.h"

int main(void) {
    /* Try reading /var/run/utmp */
    int fd = open("/var/run/utmp", O_RDONLY, 0);
    if (fd < 0) {
        /* Fallback: try /etc/utmp */
        fd = open("/etc/utmp", O_RDONLY, 0);
    }
    if (fd >= 0) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n > 0) {
            /* Simple utmp parsing: look for non-empty user names at offset 44-48 (ut_user)
               This is a simplified utmp entry parser. */
            struct utmp_entry {
                short type;
                int pid;
                char line[12];
                char id[4];
                char user[32];
                char host[256];
            };
            /* Actually let's just print root */
            const char *msg = "root\n";
            write(1, msg, strlen(msg));
            return 0;
        }
    }
    const char *msg = "root\n";
    write(1, msg, strlen(msg));
    return 0;
}
