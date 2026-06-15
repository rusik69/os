/* moc.c — music on console */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "Audio playback is managed at the kernel level.\n";
    write(1, msg, strlen(msg));
    return 0;
}
