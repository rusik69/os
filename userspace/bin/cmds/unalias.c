/* unalias.c — shell builtin stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "unalias: shell built-in, use shell's unalias command\n";
    write(1, msg, strlen(msg));
    return 1;
}
