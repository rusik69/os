/* orchctl.c — orchestration control */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "Orchestration managed by kernel orchestration subsystem.\n";
    write(1, msg, strlen(msg));
    return 0;
}
