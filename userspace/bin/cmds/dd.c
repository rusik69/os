/* dd.c — convert and copy a file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    const char *infile = 0, *outfile = 0;
    unsigned long bs = 512, count = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "if=", 3) == 0) infile = argv[i] + 3;
        else if (strncmp(argv[i], "of=", 3) == 0) outfile = argv[i] + 3;
        else if (strncmp(argv[i], "bs=", 3) == 0) { bs = 0; const char *s = argv[i] + 3; while (*s >= '0' && *s <= '9') { bs = bs * 10 + (*s - '0'); s++; } }
        else if (strncmp(argv[i], "count=", 6) == 0) { count = 0; const char *s = argv[i] + 6; while (*s >= '0' && *s <= '9') { count = count * 10 + (*s - '0'); s++; } }
    }
    if (!infile || !outfile) { printf("Usage: dd if=<infile> of=<outfile> bs=<blocksize> count=<n>\n"); return 1; }
    int in_fd = open(infile, O_RDONLY, 0);
    if (in_fd < 0) { printf("dd: cannot open '%s'\n", infile); return 1; }
    int out_fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { printf("dd: cannot open '%s'\n", outfile); close(in_fd); return 1; }
    char *buf = malloc(bs);
    if (!buf) { printf("dd: malloc failed\n"); close(in_fd); close(out_fd); return 1; }
    unsigned long total_in = 0, total_out = 0;
    unsigned long blocks_done = 0;
    while (count == 0 || blocks_done < count) {
        int n = read(in_fd, buf, bs);
        if (n <= 0) break;
        total_in += n;
        int w = write(out_fd, buf, n);
        if (w > 0) total_out += w;
        blocks_done++;
    }
    free(buf);
    close(in_fd);
    close(out_fd);
    printf("%lu+0 records in\n", blocks_done);
    printf("%lu+0 records out\n", blocks_done);
    printf("%lu bytes copied\n", total_out);
    return 0;
}
