/* ulimit.c — ulimit stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "ulimit: not supported\n";
    write(1, msg, strlen(msg));
    return 1;
}
