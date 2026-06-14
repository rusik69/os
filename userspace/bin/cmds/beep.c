/* beep.c — PC speaker beep (print bell character) */
#include "unistd.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char bell = 0x07;
    write(1, &bell, 1);
    return 0;
}
