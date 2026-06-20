#include "dos.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: dos <program.com|program.exe>\n");
        return 1;
    }
    extern int dos_exec(const char *path);
    return dos_exec(argv[1]);
}
