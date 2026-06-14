/* useradd.c — add user stub */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: useradd <username>\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    (void)argv;
    const char *msg = "useradd: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
