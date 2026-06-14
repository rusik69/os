/* rawsend.c — raw socket send stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "rawsend: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
