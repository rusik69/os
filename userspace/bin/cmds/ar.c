/* ar.c — archive creation stub */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 3) {
        const char *msg = "usage: ar rcs <archive> <files>...\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    const char *msg = "ar: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
