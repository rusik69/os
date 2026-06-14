/* fgconsole.c — print foreground console number */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    write(1, "0\n", 2);
    return 0;
}
