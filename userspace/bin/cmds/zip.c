/* zip.c — compress stub */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: zip <archive> <file>...\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    (void)argv;
    const char *msg = "zip: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
