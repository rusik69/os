/* tmux.c — terminal multiplexer */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "Use kernel shell job control (&, fg, bg) for terminal multiplexing.\n";
    write(1, msg, strlen(msg));
    return 0;
}
