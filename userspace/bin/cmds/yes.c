/* yes.c — print "y" repeatedly */

#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    const char *msg = "y";
    if (argc > 1) msg = argv[1];
    unsigned long len = strlen(msg);
    while (1) {
        write(1, msg, len);
        write(1, "\n", 1);
    }
    return 0;
}
