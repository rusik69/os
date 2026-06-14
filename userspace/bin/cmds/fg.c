/* fg.c — bring job to foreground (stub) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *msg = "fg: not supported\n";
    write(1, msg, strlen(msg));
    return 1;
}
