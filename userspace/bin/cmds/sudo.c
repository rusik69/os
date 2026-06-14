/* sudo.c — superuser do stub */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: sudo <command>\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    (void)argv;
    const char *msg = "sudo: not yet implemented (posix_spawn chaining)\n";
    write(1, msg, strlen(msg));
    return 1;
}
