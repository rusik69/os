/* cmos.c — CMOS hardware clock: not supported */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *msg = "cmos: not supported\n";
    write(1, msg, strlen(msg));
    return 1;
}
