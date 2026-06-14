/* env.c — print environment variables */

#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* environ is not available in this environment */
    return 0;
}
