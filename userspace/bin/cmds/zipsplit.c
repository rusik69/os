/* zipsplit.c — split zip stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "zipsplit: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
