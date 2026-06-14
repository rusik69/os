/* logout.c — logout stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "logout\n";
    write(1, msg, strlen(msg));
    return 0;
}
