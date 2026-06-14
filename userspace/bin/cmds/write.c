/* write.c — write to another user's terminal (stub) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: write <user> [tty]\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    (void)argv;
    const char *msg = "write: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
