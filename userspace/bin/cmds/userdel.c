/* userdel.c — delete user stub */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: userdel <username>\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    (void)argv;
    const char *msg = "userdel: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
