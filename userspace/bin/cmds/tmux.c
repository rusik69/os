/* tmux.c — terminal multiplexer stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "tmux: not available in this build\n";
    write(1, msg, strlen(msg));
    return 1;
}
