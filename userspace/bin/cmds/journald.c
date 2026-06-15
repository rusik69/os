/* journald.c — kernel journal service */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "journald is a kernel service. Use 'journalctl' to query the journal.\n";
    write(1, msg, strlen(msg));
    return 0;
}
