/* stdbuf_pipe.c — buffer control for pipes stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "stdbuf_pipe: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
