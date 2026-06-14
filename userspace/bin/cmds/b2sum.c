/* b2sum.c — BLAKE2 checksum stub (calls sha256sum equivalent) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *msg = "b2sum: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
