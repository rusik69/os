/* diff3.c — three-way diff */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

#define MAX_LINES 65536
#define MAX_LINE_LEN 4096

/* Read a file into an array of lines */
static char **read_lines(const char *path, int *num_lines) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    unsigned long size = (unsigned long)st.st_size;
    char *data = malloc(size + 1);
    if (!data) { close(fd); return NULL; }

    unsigned long total = 0;
    while (total < size) {
        int n = read(fd, data + total, size - total);
        if (n <= 0) break;
        total += (unsigned long)n;
    }
    data[total] = 0;
    close(fd);

    /* Count lines */
    int count = 0;
    for (unsigned long i = 0; i < total; i++) {
        if (data[i] == '\n') count++;
    }
    if (total > 0 && data[total - 1] != '\n') count++;

    if (count > MAX_LINES) count = MAX_LINES;

    char **lines = malloc(sizeof(char *) * (count + 1));
    if (!lines) { free(data); return NULL; }

    int idx = 0;
    char *p = data;
    while (*p) {
        char *next = strchr(p, '\n');
        unsigned long len;
        if (next) {
            len = (unsigned long)(next - p);
        } else {
            len = strlen(p);
        }
        if (len > MAX_LINE_LEN) len = MAX_LINE_LEN;
        lines[idx] = malloc(len + 1);
        if (!lines[idx]) {
            /* malloc failed — stop reading */
            free(data);
            *num_lines = idx;
            return lines;
        }
        memcpy(lines[idx], p, len);
        lines[idx][len] = 0;
        idx++;
        if (next) p = next + 1;
        else break;
        if (idx >= count) break;
    }
    lines[idx] = NULL;
    *num_lines = idx;

    free(data);
    return lines;
}

static void free_lines(char **lines, int n) {
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

/* Simple longest-common-subsequence diff — not used in this implementation */

int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("usage: diff3 <mine> <older> <yours>\n");
        return 1;
    }

    const char *mine_path = argv[1];
    const char *older_path = argv[2];
    const char *yours_path = argv[3];

    int nmine, nolder, nyours;
    char **mine = read_lines(mine_path, &nmine);
    char **older = read_lines(older_path, &nolder);
    char **yours = read_lines(yours_path, &nyours);

    if (!mine || !older || !yours) {
        printf("diff3: error reading files\n");
        if (mine) free_lines(mine, nmine);
        if (older) free_lines(older, nolder);
        if (yours) free_lines(yours, nyours);
        return 1;
    }

    /* Simple three-way merge marker output */
    int mi = 0, oi = 0, yi = 0;

    while (mi < nmine || oi < nolder || yi < nyours) {
        /* Check if lines are the same in all three */
        if (mi < nmine && oi < nolder && yi < nyours &&
            strcmp(mine[mi], older[oi]) == 0 &&
            strcmp(mine[mi], yours[yi]) == 0) {
            /* All three agree */
            printf("%s\n", mine[mi]);
            mi++; oi++; yi++;
        } else if (oi < nolder && yi < nyours &&
                   strcmp(older[oi], yours[yi]) == 0) {
            /* Older and yours agree, mine differs */
            printf("<<<<<<< %s\n", mine_path);
            while (mi < nmine && (oi >= nolder || strcmp(mine[mi], older[oi]) != 0)) {
                printf("%s\n", mine[mi]);
                mi++;
            }
            printf("=======\n");
            /* Print the agreed version from older/yours */
            if (oi < nolder) {
                printf("%s\n", older[oi]);
                oi++; yi++;
            }
            printf(">>>>>>> %s\n", yours_path);
        } else if (mi < nmine && oi < nolder &&
                   strcmp(mine[mi], older[oi]) == 0) {
            /* Mine and older agree, yours differs */
            printf("<<<<<<< %s\n", mine_path);
            printf("%s\n", mine[mi]);
            printf("=======\n");
            while (yi < nyours && (oi >= nolder || strcmp(yours[yi], older[oi]) != 0)) {
                printf("%s\n", yours[yi]);
                yi++;
            }
            printf(">>>>>>> %s\n", yours_path);
            mi++; oi++;
        } else if (mi < nmine && yi < nyours &&
                   strcmp(mine[mi], yours[yi]) == 0) {
            /* Mine and yours agree, older differs */
            while (oi < nolder && (mi >= nmine || strcmp(older[oi], mine[mi]) != 0)) {
                printf("%s\n", older[oi]);
                oi++;
            }
            printf("%s\n", mine[mi]);
            mi++; oi++; yi++;
        } else {
            /* All three differ — output conflict */
            printf("<<<<<<< %s\n", mine_path);
            if (mi < nmine) { printf("%s\n", mine[mi]); mi++; }
            printf("||||||| %s\n", older_path);
            if (oi < nolder) { printf("%s\n", older[oi]); oi++; }
            printf("=======\n");
            if (yi < nyours) { printf("%s\n", yours[yi]); yi++; }
            printf(">>>>>>> %s\n", yours_path);
        }
    }

    free_lines(mine, nmine);
    free_lines(older, nolder);
    free_lines(yours, nyours);
    return 0;
}
