/* csplit.c — split file based on context */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Simple line-by-line pattern matching for /regex/ */
static int match_pattern(const char *line, const char *pattern) {
    /* Pattern is like /regex/ without slashes */
    return strstr(line, pattern) != NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: csplit <file> <pattern>...\n");
        return 1;
    }

    const char *filename = argv[1];
    const char **patterns = (const char **)(argv + 2);
    int npatterns = argc - 2;

    if (npatterns < 1) {
        printf("csplit: no patterns specified\n");
        return 1;
    }

    int fd = open(filename, O_RDONLY, 0);
    if (fd < 0) {
        printf("csplit: cannot open '%s'\n", filename);
        return 1;
    }

    /* Read entire file into memory */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return 1;
    }

    unsigned long file_size = (unsigned long)st.st_size;
    char *data = malloc(file_size + 1);
    if (!data) {
        printf("csplit: out of memory\n");
        close(fd);
        return 1;
    }

    unsigned long total_read = 0;
    while (total_read < file_size) {
        int n = read(fd, data + total_read, file_size - total_read);
        if (n <= 0) break;
        total_read += (unsigned long)n;
    }
    data[total_read] = 0;
    close(fd);

    /* Split lines */
    /* Patterns: /regex/ or line-number */
    /* Find split points */
    unsigned long split_lines[256];
    int nsplit = 0;
    char *line = data;
    unsigned long line_num = 1;

    while (*line) {
        char *next = strchr(line, '\n');
        if (next) *next = 0;

        /* Check against all patterns */
        for (int pi = 0; pi < npatterns; pi++) {
            const char *pat = patterns[pi];
            if (pat[0] == '/' && pat[strlen(pat) - 1] == '/') {
                /* Regex pattern - extract content between slashes */
                char inner[256];
                unsigned long plen = strlen(pat) - 2;
                if (plen >= sizeof(inner)) plen = sizeof(inner) - 1;
                memcpy(inner, pat + 1, plen);
                inner[plen] = 0;
                if (match_pattern(line, inner)) {
                    if (nsplit < 256) split_lines[nsplit++] = line_num;
                }
            } else {
                /* Line number pattern */
                unsigned long target = 0;
                const char *p = pat;
                while (*p >= '0' && *p <= '9') {
                    target = target * 10 + (*p - '0');
                    p++;
                }
                if (*p == 0 && target > 0 && target == line_num) {
                    if (nsplit < 256) split_lines[nsplit++] = line_num;
                }
            }
        }

        if (next) {
            line = next + 1;
            line_num++;
        } else break;
    }

    /* Reset data pointer */
    line = data;
    line_num = 1;

    /* Write split files */
    int out_fd = -1;
    char out_name[32];
    int file_idx = 0;
    int split_idx = 0;

    /* Create first output file */
    snprintf(out_name, sizeof(out_name), "xx%02d", file_idx++);
    out_fd = open(out_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        printf("csplit: cannot create '%s'\n", out_name);
        free(data);
        return 1;
    }

    while (*line) {
        char *next = strchr(line, '\n');
        unsigned long line_len;
        if (next) {
            line_len = (unsigned long)(next - line);
        } else {
            line_len = strlen(line);
        }

        /* Check if we need to split at this line */
        if (split_idx < nsplit && line_num == split_lines[split_idx]) {
            /* Close current file and open next */
            close(out_fd);
            snprintf(out_name, sizeof(out_name), "xx%02d", file_idx++);
            out_fd = open(out_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd < 0) {
                printf("csplit: cannot create '%s'\n", out_name);
                free(data);
                return 1;
            }
            split_idx++;
        }

        /* Write line to current file */
        write(out_fd, line, line_len);
        write(out_fd, "\n", 1);

        if (next) {
            line = next + 1;
            line_num++;
        } else break;
    }

    if (out_fd >= 0) close(out_fd);

    printf("csplit: created %d files\n", file_idx);
    free(data);
    return 0;
}
