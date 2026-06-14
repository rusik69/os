/* bg.c — put job in background (stub) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *msg = "bg: not supported\n";
    write(1, msg, strlen(msg));
    return 1;
}
