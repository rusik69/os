/* ncdu.c — disk usage analyzer stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "ncdu: not available in this build\n";
    write(1, msg, strlen(msg));
    return 1;
}
