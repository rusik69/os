/* tset.c — terminal initialization and mode setting */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

/* Terminal ioctl commands — matching kernel's termios.h */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404

/* Terminal flags for local modes */
#define ECHO      0x0008
#define ICANON    0x0002
#define ISIG      0x0001

/* Terminal flags for input modes */
#define ICRNL     0x0100
#define BRKINT    0x0002
#define INPCK     0x0020
#define ISTRIP    0x0040

/* Terminal flags for output modes */
#define OPOST     0x0001
#define ONLCR     0x0004

/* Terminal flags for control modes */
#define CS8       0x0030
#define CREAD     0x0080

/* Minimal termios struct matching kernel */
struct termios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_cc[20];
};

static void set_sane_termios(void) {
    struct termios t;
    int fd = 0; /* stdin */

    if (ioctl(fd, TCGETS, &t) < 0)
        return;

    /* Set sane terminal modes: cooked, echo on, signal handling */
    t.c_iflag |= ICRNL | BRKINT;
    t.c_oflag |= OPOST | ONLCR;
    t.c_cflag |= CS8 | CREAD;
    t.c_lflag |= ICANON | ECHO | ISIG;

    /* Default VMIN = 1, VTIME = 0 */
    t.c_cc[0] = 1;   /* VMIN  = c_cc[0] */
    t.c_cc[1] = 0;   /* VTIME = c_cc[1] */

    ioctl(fd, TCSETSF, &t);
}

static void set_raw_termios(void) {
    struct termios t;
    int fd = 0;

    if (ioctl(fd, TCGETS, &t) < 0)
        return;

    /* Raw mode: no processing */
    t.c_iflag = 0;
    t.c_oflag = 0;
    t.c_cflag = CS8 | CREAD;
    t.c_lflag = 0;
    t.c_cc[0] = 1;   /* VMIN */
    t.c_cc[1] = 0;   /* VTIME */

    ioctl(fd, TCSETSF, &t);
}

int main(int argc, char *argv[]) {
    /* Default: set sane (cooked) terminal modes */
    if (argc == 1) {
        set_sane_termios();
        printf("tset: terminal initialized (cooked mode)\n");
        return 0;
    }

    if (strcmp(argv[1], "-raw") == 0 || strcmp(argv[1], "raw") == 0) {
        set_raw_termios();
        printf("tset: terminal set to raw mode\n");
        return 0;
    }

    if (strcmp(argv[1], "-cooked") == 0 || strcmp(argv[1], "cooked") == 0 ||
        strcmp(argv[1], "-sane") == 0 || strcmp(argv[1], "sane") == 0) {
        set_sane_termios();
        printf("tset: terminal initialized (cooked mode)\n");
        return 0;
    }

    printf("usage: tset          (set sane/cooked terminal modes)\n");
    printf("       tset raw      (set raw terminal mode)\n");
    printf("       tset cooked   (set cooked terminal mode)\n");
    return 1;
}
