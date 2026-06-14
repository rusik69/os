/* tsort.c — topological sort */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_NODES 256
#define MAX_NAME 64

static char names[MAX_NODES][MAX_NAME];
static int edges[MAX_NODES][MAX_NODES]; /* adjacency matrix */
static int indeg[MAX_NODES];
static int nnodes = 0;

static int find_or_add(const char *name) {
    for (int i = 0; i < nnodes; i++)
        if (strcmp(names[i], name) == 0) return i;
    if (nnodes >= MAX_NODES) return -1;
    int j = 0;
    while (name[j] && j < MAX_NAME - 1) { names[nnodes][j] = name[j]; j++; }
    names[nnodes][j] = '\0';
    indeg[nnodes] = 0;
    for (int k = 0; k < MAX_NODES; k++) edges[nnodes][k] = 0;
    return nnodes++;
}

int main(int argc, char *argv[]) {
    int fd = STDIN_FILENO;
    int should_close = 0;
    if (argc > 1) {
        fd = open(argv[1], O_RDONLY, 0);
        if (fd < 0) { printf("tsort: %s: No such file\n", argv[1]); return 1; }
        should_close = 1;
    }
    char buf[4096];
    char line[256];
    int line_len = 0, nread;
    while ((nread = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) {
                    /* Split on whitespace */
                    char *first = line, *second = 0;
                    char *p = line;
                    while (*p && *p != ' ' && *p != '\t') p++;
                    if (*p) { *p = '\0'; p++; while (*p == ' ' || *p == '\t') p++; second = p; }
                    if (second && *second) {
                        int a = find_or_add(first);
                        int b = find_or_add(second);
                        if (a >= 0 && b >= 0 && !edges[a][b]) {
                            edges[a][b] = 1;
                            indeg[b]++;
                        }
                    }
                }
                line_len = 0;
            } else if (line_len < 255) {
                line[line_len++] = buf[i];
            }
        }
    }
    if (should_close) close(fd);
    /* Kahn's algorithm */
    int queue[MAX_NODES], head = 0, tail = 0;
    for (int i = 0; i < nnodes; i++)
        if (indeg[i] == 0) queue[tail++] = i;
    int visited = 0;
    while (head < tail) {
        int v = queue[head++];
        printf("%s\n", names[v]);
        visited++;
        for (int w = 0; w < nnodes; w++) {
            if (edges[v][w]) {
                if (--indeg[w] == 0)
                    queue[tail++] = w;
            }
        }
    }
    if (visited != nnodes) {
        printf("tsort: cycle detected\n");
        return 1;
    }
    return 0;
}
