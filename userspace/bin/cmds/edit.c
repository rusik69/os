/* edit.c — stub */
#include "unistd.h"
#include "string.h"

int main(void) {
    const char *msg = "edit: not available, use ed or nano\n";
    write(1, msg, strlen(msg));
    return 1;
}
