/* zipnote.c — zip comment stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "zipnote: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
