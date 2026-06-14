/* basenc.c — various encodings (stub, falls through to base64/base32) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        const char *msg = "usage: basenc --base64|--base32 [file]\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    const char *msg = "basenc: not yet implemented\n";
    write(1, msg, strlen(msg));
    return 1;
}
