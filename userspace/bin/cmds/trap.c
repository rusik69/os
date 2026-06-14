/* trap.c — shell builtin stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "trap: shell built-in, use shell's trap command\n";
    write(1, msg, strlen(msg));
    return 1;
}
