/* fsfreeze.c — freeze/unfreeze filesystem */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* FIFREEZE / FITHAW ioctls (Linux values) */
/* struct { uint64_t start; uint64_t len; uint32_t flags; } */
/* Actually FIFREEZE is _IOWR('X', 119, ...) and FITHAW is _IOWR('X', 120, ...) */
/* From Linux: FIFREEZE = _IOWR(0x58, 119, struct fstrim_range) */
/* But actually: FIFREEZE = _IOC(_IOC_READ|_IOC_WRITE, 0x58, 119, sizeof(int)) */
/* Let me use the raw values from Linux kernel headers: */
/* FIFREEZE = 0xC0045877  (_IOWR('X', 119, __u32))  but with 'X' = 0x58 */
/* Actually let's compute: _IOWR(0x58, 119, int) */
/* direction = 2 (IOC_INOUT), size = 4, type = 0x58, nr = 119 */
/* = (2<<30) | (4<<16) | (0x58<<8) | 119 */
/* = 0x80000000 | 0x00040000 | 0x00005800 | 0x00000077 */
/* = 0x80045877 */
#define FIFREEZE    0x80045877  /* Freeze filesystem */
/* Same for FITHAW with nr = 120 */
#define FITHAW      0x80045878  /* Thaw filesystem */

int main(int argc, char *argv[]) {
    int freeze = 0;
    int unfreeze = 0;
    const char *mountpoint = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--freeze") == 0) {
            freeze = 1;
        } else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--unfreeze") == 0) {
            unfreeze = 1;
        } else if (argv[i][0] == '-') {
            printf("fsfreeze: invalid option '%s'\n", argv[i]);
            printf("Usage: fsfreeze -f|-u <mountpoint>\n");
            return 1;
        } else {
            mountpoint = argv[i];
        }
    }

    if (!mountpoint || (!freeze && !unfreeze)) {
        printf("Usage: fsfreeze -f|-u <mountpoint>\n");
        return 1;
    }

    struct stat st;
    if (stat(mountpoint, &st) < 0) {
        printf("fsfreeze: cannot stat '%s'\n", mountpoint);
        return 1;
    }

    printf("Mountpoint: %s\n", mountpoint);

    int fd = open(mountpoint, O_RDONLY, 0);
    if (fd < 0) {
        printf("fsfreeze: cannot open '%s'\n", mountpoint);
        return 1;
    }

    int ret;
    if (freeze) {
        ret = ioctl(fd, FIFREEZE, NULL);
        if (ret < 0) {
            printf("fsfreeze: FIFREEZE not supported on '%s'\n", mountpoint);
            close(fd);
            return 1;
        }
        printf("fsfreeze: filesystem '%s' frozen\n", mountpoint);
    } else {
        ret = ioctl(fd, FITHAW, NULL);
        if (ret < 0) {
            printf("fsfreeze: FITHAW not supported on '%s'\n", mountpoint);
            close(fd);
            return 1;
        }
        printf("fsfreeze: filesystem '%s' thawed\n", mountpoint);
    }

    close(fd);
    return 0;
}
