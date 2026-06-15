/* ptx.c — produce permuted index (KWIC) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

#define MAX_LINES 65536
#define MAX_LINE_LEN 4096

/* A KWIC (KeyWord In Context) entry */
struct kwic_entry {
    char *line;       /* The full line */
    char *keyword;    /* The keyword (position within line) */
    int kw_offset;    /* Word number of keyword (0-based) */
};

/* Read file into array of lines */
static int read_lines(const char *path, char ***lines_out) {
    int fd;
    if (path) {
        fd = open(path, O_RDONLY, 0);
        if (fd < 0) return -1;
    } else {
        fd = 0; /* stdin */
    }

    struct stat st;
    unsigned long size;
    if (path && fstat(fd, &st) >= 0) {
        size = (unsigned long)st.st_size;
    } else {
        size = 65536;
    }

    char *data = malloc(size + 1);
    if (!data) { if (path) close(fd); return -1; }

    unsigned long total = 0;
    while (total < size) {
        int n = read(fd, data + total, size - total);
        if (n <= 0) break;
        total += (unsigned long)n;
        if (!path && n < (int)(size - total)) break;
    }
    data[total] = 0;
    if (path) close(fd);

    /* Count lines */
    int count = 0;
    for (unsigned long i = 0; i < total; i++) {
        if (data[i] == '\n') count++;
    }
    if (total > 0 && data[total - 1] != '\n') count++;
    if (count > MAX_LINES) count = MAX_LINES;

    char **lines = malloc(sizeof(char *) * (unsigned long)(count + 1));
    if (!lines) { free(data); return -1; }

    int idx = 0;
    char *p = data;
    while (*p && idx < count) {
        char *next = strchr(p, '\n');
        unsigned long len;
        if (next) {
            len = (unsigned long)(next - p);
        } else {
            len = strlen(p);
        }
        if (len > MAX_LINE_LEN) len = MAX_LINE_LEN;
        lines[idx] = malloc(len + 1);
        memcpy(lines[idx], p, len);
        lines[idx][len] = 0;
        idx++;
        if (next) p = next + 1;
        else break;
    }
    lines[idx] = NULL;
    *lines_out = lines;
    free(data);
    return idx;
}

static void free_lines(char **lines, int count) {
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

/* Check if char is whitespace */
static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* Count words in a line */
static int count_words(const char *line) {
    int count = 0;
    int in_word = 0;
    while (*line) {
        if (is_space(*line)) {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            count++;
        }
        line++;
    }
    return count;
}

/* Get pointer to nth word in line */
static char *get_word_pos(char *line, int n) {
    int count = 0;
    int in_word = 0;
    char *p = line;
    while (*p) {
        if (is_space(*p)) {
            in_word = 0;
        } else if (!in_word) {
            if (count == n) return p;
            in_word = 1;
            count++;
        }
        p++;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    const char *file = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            /* width = atoi(argv[++i]); */ /* Accepted but unused */
            i++;
        } else if (argv[i][0] == '-') {
            printf("ptx: invalid option '%s'\n", argv[i]);
            printf("Usage: ptx [-w n] [<file>]\n");
            return 1;
        } else {
            file = argv[i];
        }
    }

    char **lines = NULL;
    int nlines = read_lines(file, &lines);
    if (nlines < 0) {
        printf("ptx: cannot read input\n");
        return 1;
    }

    /* Collect KWIC entries */
    struct kwic_entry *entries = malloc(sizeof(struct kwic_entry) * (unsigned long)(nlines * 32));
    if (!entries) {
        free_lines(lines, nlines);
        return 1;
    }
    int nentries = 0;

    for (int li = 0; li < nlines; li++) {
        int nw = count_words(lines[li]);
        for (int wi = 0; wi < nw; wi++) {
            char *kw = get_word_pos(lines[li], wi);
            if (kw) {
                entries[nentries].line = lines[li];
                entries[nentries].keyword = kw;
                entries[nentries].kw_offset = wi;
                nentries++;
            }
        }
    }

    /* Sort entries by keyword (simple bubble sort for now) */
    for (int ei = 0; ei < nentries; ei++) {
        for (int ej = ei + 1; ej < nentries; ej++) {
            int cmp = strcmp(entries[ei].keyword, entries[ej].keyword);
            if (cmp > 0) {
                struct kwic_entry tmp = entries[ei];
                entries[ei] = entries[ej];
                entries[ej] = tmp;
            }
        }
    }

    /* Output permuted index */
    for (int ei = 0; ei < nentries; ei++) {
        printf("%s\n", entries[ei].line);
    }

    printf("ptx: %d entries from %d lines\n", nentries, nlines);
    free(entries);
    free_lines(lines, nlines);
    return 0;
}
