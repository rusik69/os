#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mktemp [TEMPLATE]\n");
        return 1;
    }
    char template[256];
    strncpy(template, argv[1], sizeof(template) - 1);
    template[sizeof(template) - 1] = 0;
    /* Simple template: replace X's with random hex */
    int fd = open("/dev/urandom", 0, 0);
    unsigned char rbuf[8];
    int have_random = 0;
    if (fd >= 0) {
        if (read(fd, rbuf, 8) > 0) have_random = 1;
        close(fd);
    }
    int xcount = 0;
    for (int i = 0; template[i]; i++) {
        if (template[i] == 'X') xcount++;
    }
    if (xcount == 0) {
        printf("mktemp: template must contain X's\n");
        return 1;
    }
    int xi = 0;
    for (int i = 0; template[i]; i++) {
        if (template[i] == 'X') {
            unsigned char r = (have_random ? rbuf[xi % 8] : (unsigned char)(getpid() * (xi+1)));
            template[i] = "0123456789abcdef"[r & 0xf];
            xi++;
        }
    }
    /* Create the file */
    int f = open(template, O_CREAT | O_WRONLY, 0600);
    if (f < 0) {
        printf("mktemp: cannot create %s\n", template);
        return 1;
    }
    close(f);
    printf("%s\n", template);
    return 0;
}
