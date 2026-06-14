/* passwd.c — password stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "passwd: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
