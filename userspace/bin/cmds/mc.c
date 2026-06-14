/* mc.c — midnight commander stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "mc: Midnight Commander not available in this build\n";
    write(1, msg, strlen(msg));
    return 1;
}
