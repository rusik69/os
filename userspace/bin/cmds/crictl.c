/* crictl.c — container runtime interface stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "crictl: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
