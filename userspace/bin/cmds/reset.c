/* reset.c — reset terminal */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Send terminal reset escape sequence */
    write(STDOUT_FILENO, "\033c", 2);    /* reset to initial state */
    write(STDOUT_FILENO, "\033[2J", 4);  /* clear screen */
    write(STDOUT_FILENO, "\033[H", 3);   /* cursor home */
    write(STDOUT_FILENO, "\033[?25h", 6); /* show cursor */
    return 0;
}
