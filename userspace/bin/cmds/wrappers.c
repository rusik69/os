/* wrappers.c — internal tool */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "wrappers: internal tool\n";
    write(1, msg, strlen(msg));
    return 0;
}
