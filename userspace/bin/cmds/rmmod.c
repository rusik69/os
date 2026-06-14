/* rmmod.c — module remove (stub) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: rmmod <module>\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    (void)argv;
    const char *msg = "rmmod: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
