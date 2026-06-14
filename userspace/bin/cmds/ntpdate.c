/* ntpdate.c — NTP time sync stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "ntpdate: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
