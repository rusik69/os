/* ctr.c — containerd control */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "containerd operations available via kernel shell 'ctr' command.\n";
    write(1, msg, strlen(msg));
    return 0;
}
