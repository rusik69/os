/* su.c — switch user stub */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *msg = "su: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
