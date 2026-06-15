/* mouse.c — mouse input control */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    /* Try different mouse device paths */
    const char *devs[] = {
        "/dev/mouse",
        "/dev/input/mice",
        "/dev/input/mouse0",
        NULL
    };

    int fd = -1;
    for (int i = 0; devs[i]; i++) {
        fd = open(devs[i], O_RDONLY, 0);
        if (fd >= 0) {
            printf("mouse: opened %s\n", devs[i]);
            break;
        }
    }

    if (fd < 0) {
        printf("mouse: no mouse device found\n");
        printf("mouse: kernel mouse driver may not be loaded\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "test") == 0) {
        /* Read and display mouse data */
        printf("mouse: reading events (Ctrl-C to stop)...\n");
        unsigned char buf[8];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            for (int i = 0; i < n; i++) {
                char hex[4];
                snprintf(hex, sizeof(hex), "%02x ", buf[i]);
                write(1, hex, 3);
            }
            write(1, "\n", 1);
        }
        close(fd);
        return 0;
    }

    printf("usage: mouse             (show mouse status)\n");
    printf("       mouse test        (read and display mouse events)\n");
    close(fd);
    return 0;
}
