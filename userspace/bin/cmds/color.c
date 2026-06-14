/* color.c — set ANSI console color */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Reset to default colors */
    const char *reset = "\033[0m";
    write(1, reset, strlen(reset));
    return 0;
}
