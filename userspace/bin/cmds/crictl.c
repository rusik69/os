/* crictl.c — container runtime interface */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "Container runtime managed by kernel's container subsystem (src/container/). Use 'crictl' from kernel shell.\n";
    write(1, msg, strlen(msg));
    return 0;
}
