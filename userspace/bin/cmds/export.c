/* export.c — shell builtin stub */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *msg = "export: shell built-in, use shell's export command\n";
    write(1, msg, strlen(msg));
    return 1;
}
