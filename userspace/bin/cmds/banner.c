/* banner.c — print banner text in a box */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        const char *msg = "usage: banner <text>...\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    /* Print text in a simple box */
    unsigned long len = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) len++;
        len += strlen(argv[i]);
    }
    /* top border */
    write(1, "+-", 2);
    for (unsigned long i = 0; i < len; i++) write(1, "-", 1);
    write(1, "-+\n", 3);
    /* text line */
    write(1, "| ", 2);
    for (int i = 1; i < argc; i++) {
        if (i > 1) write(1, " ", 1);
        write(1, argv[i], strlen(argv[i]));
    }
    write(1, " |\n", 3);
    /* bottom border */
    write(1, "+-", 2);
    for (unsigned long i = 0; i < len; i++) write(1, "-", 1);
    write(1, "-+\n", 3);
    return 0;
}
