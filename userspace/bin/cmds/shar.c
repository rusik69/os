/* shar.c — create shell archives with uuencode encoding */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* Uuencode: 3 bytes -> 4 chars, 6-bit each, offset by 32.
   Max 45 bytes per line -> 60 encoded chars. */
#define ENC_LINE_BYTES  45
#define ENC_LINE_CHARS  60

static void uuencode_line(const unsigned char *data, int count, int outfd) {
    char line[64];
    int linepos = 0;

    /* Length character: count + 32 */
    line[linepos++] = (char)(count + 32);

    int i = 0;
    while (i < count) {
        unsigned char b0 = data[i];
        unsigned char b1 = (i + 1 < count) ? data[i + 1] : 0;
        unsigned char b2 = (i + 2 < count) ? data[i + 2] : 0;

        line[linepos++] = (char)(((b0 >> 2) & 0x3f) + 32);
        line[linepos++] = (char)((((b0 << 4) | (b1 >> 4)) & 0x3f) + 32);
        line[linepos++] = (char)((((b1 << 2) | (b2 >> 6)) & 0x3f) + 32);
        line[linepos++] = (char)((b2 & 0x3f) + 32);

        i += 3;
    }
    line[linepos++] = '\n';
    write(outfd, line, linepos);
}

static int shar_one_file(const char *path, int outfd) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("shar: cannot open '%s'\n", path);
        return 1;
    }

    /* Get file size and mode */
    struct stat st;
    if (stat(path, &st) < 0) {
        close(fd);
        printf("shar: cannot stat '%s'\n", path);
        return 1;
    }

    /* Basename */
    const char *bname = path;
    const char *slash = strrchr(path, '/');
    if (slash) bname = slash + 1;

    /* Write file header */
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "#\n# File: %s\n# Size: %llu bytes\n# Mode: %o\n#\n",
        bname, (unsigned long long)st.st_size, (unsigned int)(st.st_mode & 07777));
    write(outfd, header, hlen);

    /* Write uuencode begin line */
    char beginline[128];
    int blen = snprintf(beginline, sizeof(beginline),
        "uudecode << 'SHAR_EOF'\n");
    write(outfd, beginline, blen);
    blen = snprintf(beginline, sizeof(beginline),
        "begin %o %s\n", (unsigned int)(st.st_mode & 07777), bname);
    write(outfd, beginline, blen);

    /* Read file and uuencode */
    unsigned char buf[ENC_LINE_BYTES];
    int n;
    while ((n = read(fd, buf, ENC_LINE_BYTES)) > 0) {
        uuencode_line(buf, n, outfd);
    }
    close(fd);

    /* Write end marker and heredoc terminator */
    write(outfd, "end\n", 4);
    write(outfd, "SHAR_EOF\n", 9);

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: shar <file...>\n");
        return 1;
    }

    /* Shell script header */
    write(1, "#!/bin/sh\n", 10);
    write(1, "# This is a shell archive (created by shar)\n", 44);
    write(1, "# Extract with: sh thisfile\n\n", 29);

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (shar_one_file(argv[i], 1) != 0)
            ret = 1;
    }

    write(1, "\n# End of shell archive\n", 23);
    return ret;
}
