#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* patch: apply a unified diff to a file (stub implementation)
   This simplified version applies simple context diff hunks to a target file.
   It parses lines starting with +/- and applies changes. */

#define BUF_SIZE 4096

static char *slurp(const char *path, long *size) {
    int fd = open(path, 0, 0);
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
            if (!nd) { close(fd); free(data); return NULL; }
            data = nd;
        }
    }
    close(fd);
    data[len] = '\0';
    *size = len;
    return data;
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
    char *out = malloc(fsize + psize + 1);
    long olen = 0;
    char *flines[8192], *plines[4096];
    int nfl = 0, npl = 0;
    flines[nfl++] = fdata;
    for (char *p = fdata; *p; p++) if (*p == '\n') { *p = '\0'; if (*(p+1)) flines[nfl++] = p+1; }
    plines[npl++] = pdata;
    for (char *p = pdata; *p; p++) if (*p == '\n') { *p = '\0'; if (*(p+1)) plines[npl++] = p+1; }
    int fi = 0;
    for (int pi = 0; pi < npl; pi++) {
        char *line = plines[pi];
        if (line[0] == 'd' && line[1] == 'i' && line[2] == 'f' && line[3] == 'f')
            continue; /* skip diff header */
        if (line[0] == '-' && line[1] == '-') continue; /* skip --- line */
        if (line[0] == '+' && line[1] == '+') continue; /* skip +++ line */
        if (line[0] == '@') continue; /* skip hunk header */
        if (line[0] == ' ') {
            /* context line - copy from file */
            if (fi < nfl) {
                int len = strlen(flines[fi]);
                memcpy(out + olen, flines[fi], len);
                olen += len;
                out[olen++] = '\n';
                fi++;
            }
        } else if (line[0] == '-') {
            /* removal - skip line in file */
            fi++;
        } else if (line[0] == '+') {
            /* addition - add new line */
            int len = strlen(line + 1);
            if (len > 0) {
                memcpy(out + olen, line + 1, len);
                olen += len;
                out[olen++] = '\n';
            }
        } else {
            /* copy remaining from file */
            while (fi < nfl) {
                int len = strlen(flines[fi]);
                memcpy(out + olen, flines[fi], len);
                olen += len;
                out[olen++] = '\n';
                fi++;
            }
        }
    }
    while (fi < nfl) {
        int len = strlen(flines[fi]);
        memcpy(out + olen, flines[fi], len);
        olen += len;
        out[olen++] = '\n';
        fi++;
    }
    out[olen] = '\0';
    /* write back to target */
    int fd = open(target, 1, 0); /* O_WRONLY */
    if (fd < 0) { printf("patch: cannot write %s\n", target); free(fdata); free(pdata); free(out); return 1; }
    write(fd, out, olen);
    close(fd);
    free(fdata);
    free(pdata);
    free(out);
    return 0;
}
