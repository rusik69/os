/* unset.c — shell builtin stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "unset: shell built-in, use shell's unset command\n";
    write(1, msg, strlen(msg));
    return 1;
}
