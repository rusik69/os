/* shred.c — overwrite file securely */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static unsigned int rand_state = 1;

static void my_srand(unsigned int seed) {
    rand_state = seed;
}

static int my_rand(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return (rand_state >> 16) & 0x7FFF;
}

int main(int argc, char *argv[]) {
    int passes = 3;
    int optind = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        if (argc > 2) {
            passes = atoi(argv[2]);
            optind = 3;
        } else {
            optind = 2;
        }
    }
    if (argc < optind + 1) {
        printf("Usage: shred [-n PASSES] FILE...\n");
        return 1;
    }
    /* Seed RNG */
    struct timespec ts;
    clock_gettime(0, &ts);
    unsigned int seed = (unsigned int)(ts.tv_sec ^ ts.tv_nsec);
    seed ^= (unsigned int)getpid();
    my_srand(seed);
    (void)my_srand;
    (void)my_rand;
    for (int i = optind; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY, 0);
        if (fd < 0) {
            printf("shred: cannot open '%s'\n", argv[i]);
            continue;
        }
        /* Get file size */
        struct stat st;
        if (fstat(fd, &st) < 0) {
            printf("shred: cannot stat '%s'\n", argv[i]);
            close(fd);
            continue;
        }
        unsigned long size = st.st_size;
        /* Overwrite with random data */
        for (int pass = 0; pass < passes; pass++) {
            lseek(fd, 0, SEEK_SET);
            unsigned long remaining = size;
            char buf[512];
            while (remaining > 0) {
                unsigned long chunk = remaining < 512 ? remaining : 512;
                for (unsigned long j = 0; j < chunk; j++)
                    buf[j] = (char)(my_rand() & 0xFF);
                write(fd, buf, chunk);
                remaining -= chunk;
            }
        }
        /* Final overwrite with zeros */
        lseek(fd, 0, SEEK_SET);
        unsigned long remaining = size;
        char zbuf[512];
        memset(zbuf, 0, 512);
        while (remaining > 0) {
            unsigned long chunk = remaining < 512 ? remaining : 512;
            write(fd, zbuf, chunk);
            remaining -= chunk;
        }
        close(fd);
        printf("shred: %s overwritten (%d passes + zero)\n", argv[i], passes);
    }
    return 0;
}
