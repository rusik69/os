/* udevd.c — device event daemon */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "udevd is handled by the kernel's devfs/sysfs subsystem.\n";
    write(1, msg, strlen(msg));
    return 0;
}
