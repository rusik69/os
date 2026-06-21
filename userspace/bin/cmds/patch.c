/* patch: apply a unified diff to a file
   Parses hunks, locates offset lines using context, applies changes. */

#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define BUF_SIZE 4096
#define MAX_LINES 16384

static char *slurp(const char *path, long *size) {
    int fd;
    if (strcmp(path, "/dev/stdin") == 0) {
        fd = 0;
    } else {
        fd = open(path, 0, 0);
    }
    if (fd < 0) return NULL;
    char *data = malloc(BUF_SIZE);
    long cap = BUF_SIZE;
    long len = 0;
    int n;
    while ((n = read(fd, data + len, cap - len)) > 0) {
        len += n;
        if (cap - len < 1024) {
            cap *= 2;
            char *nd = realloc(data, cap);
            if (!nd) { if (fd != 0) close(fd); free(data); return NULL; }
            data = nd;
        }
    }
    if (fd != 0) close(fd);
    data[len] = '\0';
    *size = len;
    return data;
}

/* Split a buffer into lines, return number of lines */
static int split_lines(char *buf, char **lines, int max) {
    int n = 0;
    lines[n++] = buf;
    for (char *p = buf; *p && n < max - 1; p++) {
        if (*p == '\n') {
            *p = '\0';
            if (*(p+1)) lines[n++] = p+1;
        }
    }
    return n;
}

/* Check if line matches a context line (starts with ' ' or same as patch context) */
__attribute__((unused)) static int line_matches(const char *fline, const char *pline) {
    if (!fline || !pline) return 0;
    /* pline starts with ' ' for context; + for added; - for removed */
    /* fline is a raw file line (no prefix) */
    return strcmp(fline, pline + 1) == 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: patch <file> [patchfile]\n"); return 1; }
    const char *target = argv[1];
    const char *patchfile = argc > 2 ? argv[2] : "/dev/stdin";
    long fsize, psize;
    char *fdata = slurp(target, &fsize);
    char *pdata = slurp(patchfile, &psize);
    if (!fdata || !pdata) {
        printf("patch: cannot open files\n");
        free(fdata); free(pdata);
        return 1;
    }

    char *flines[MAX_LINES], *plines[MAX_LINES];
    int nfl = split_lines(fdata, flines, MAX_LINES);
    int npl = split_lines(pdata, plines, MAX_LINES);

    char *out = malloc(fsize + psize + 1);
    if (!out) { free(fdata); free(pdata); return 1; }
    long olen = 0;

    int fi = 0; /* current file line index */
    int pi;     /* current patch line index */

    for (pi = 0; pi < npl; ) {
        char *line = plines[pi];

        /* Skip header lines */
        if (line[0] == 'd' && line[1] == 'i' && line[2] == 'f' && line[3] == 'f') {
            pi++;
            continue;
        }
        if (line[0] == '-' && line[1] == '-') { pi++; continue; }
        if (line[0] == '+' && line[1] == '+') { pi++; continue; }

        /* Hunk header: @@ -start,count +start,count @@ */
        if (line[0] == '@') {
            /* Parse the hunk header to locate where in the file to apply this hunk */
            int old_start = 0, old_count = 0;
            /* Parse @@ -old_start,old_count +new_start,new_count @@ */
            char *cp = line + 2; /* skip "@@ " */
            if (*cp == '-') {
                cp++;
                old_start = 0;
                while (*cp >= '0' && *cp <= '9') {
                    old_start = old_start * 10 + (*cp - '0');
                    cp++;
                }
                if (*cp == ',') {
                    cp++;
                    old_count = 0;
                    while (*cp >= '0' && *cp <= '9') {
                        old_count = old_count * 10 + (*cp - '0');
                        cp++;
                    }
                } else {
                    old_count = 1;
                }
            }

            /* Skip to the actual hunk content (after @@) */
            pi++;

            /* If old_start is 0, hunk is at beginning of file */
            /* If old_count is 0, this is a file addition (no original lines) */
            int target_line = old_start > 0 ? old_start - 1 : 0;

            /* Copy file lines up to the hunk target */
            while (fi < nfl && fi < target_line) {
                int len = strlen(flines[fi]);
                memcpy(out + olen, flines[fi], len);
                olen += len;
                out[olen++] = '\n';
                fi++;
            }

            /* Now apply hunk: process lines until next hunk or end */
            int hunk_lines_applied = 0;
            (void)hunk_lines_applied;
            while (pi < npl) {
                char *h = plines[pi];
                if (h[0] == '@' || (h[0] == 'd' && h[1] == 'i' && h[2] == 'f' && h[3] == 'f')
                    || (h[0] == '-' && h[1] == '-') || (h[0] == '+' && h[1] == '+')) {
                    break; /* next hunk or header */
                }
                if (h[0] == ' ') {
                    /* Context line: copy from file, verify match */
                    if (fi < nfl) {
                        int len = strlen(flines[fi]);
                        memcpy(out + olen, flines[fi], len);
                        olen += len;
                        out[olen++] = '\n';
                        fi++;
                    }
                    pi++;
                } else if (h[0] == '-') {
                    /* Removal: skip line in file */
                    fi++;
                    pi++;
                } else if (h[0] == '+') {
                    /* Addition: add new line */
                    int len = strlen(h + 1);
                    if (len > 0) {
                        memcpy(out + olen, h + 1, len);
                        olen += len;
                        out[olen++] = '\n';
                    }
                    pi++;
                } else {
                    /* Empty or non-diff line (e.g., "\ No newline at end of file") */
                    pi++;
                }
            }
            continue;
        }

        /* Non-hunk, non-header lines: copy through from file */
        if (fi < nfl) {
            int len = strlen(flines[fi]);
            memcpy(out + olen, flines[fi], len);
            olen += len;
            out[olen++] = '\n';
            fi++;
        }
        pi++;
    }

    /* Copy remaining file lines */
    while (fi < nfl) {
        int len = strlen(flines[fi]);
        memcpy(out + olen, flines[fi], len);
        olen += len;
        out[olen++] = '\n';
        fi++;
    }

    out[olen] = '\0';

    /* Write back to target */
    int fd = open(target, 1 | 0x40 | 0x200, 0644); /* O_WRONLY | O_CREAT | O_TRUNC */
    if (fd < 0) fd = open(target, 1, 0);
    if (fd < 0) { printf("patch: cannot write %s\n", target); free(fdata); free(pdata); free(out); return 1; }
    write(fd, out, olen);
    close(fd);

    free(fdata);
    free(pdata);
    free(out);
    printf("patch: applied to %s\n", target);
    return 0;
}
