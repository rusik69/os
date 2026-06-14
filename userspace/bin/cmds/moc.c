/* moc.c — music on console stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "moc: Music On Console not available in this build\n";
    write(1, msg, strlen(msg));
    return 1;
}
