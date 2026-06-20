/* cmd_tsort.c -- Topological sort */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

#define MAX_NODES 512
#define MAX_EDGES 2048

struct edge {
    int from;
    int to;
};

/* Nodes store name and in-degree count */
struct node {
    char name[64];
    int indegree;
    int outdegree;
    int edge_start; /* index into edge list */
};

static int find_node(struct node *nodes, int *ncount, const char *name) {
    for (int i = 0; i < *ncount; i++) {
        if (strcmp(nodes[i].name, name) == 0)
            return i;
    }
    if (*ncount >= MAX_NODES) return -1;
    strncpy(nodes[*ncount].name, name, 63);
    nodes[*ncount].name[63] = '\0';
    nodes[*ncount].indegree = 0;
    nodes[*ncount].outdegree = 0;
    nodes[*ncount].edge_start = -1;
    return (*ncount)++;
}

int cmd_tsort(int argc, char **argv) {
    static struct node nodes[MAX_NODES];
    int ncount = 0;
    static struct edge edges[MAX_EDGES];
    int ecount = 0;

    /* Read input: from file or stdin */
    static char fbuf[32768];
    uint32_t fsize = 0;

    if (argc >= 2) {
        char path[64];
        const char *fn = argv[1];
        if (fn[0] != '/') { path[0] = '/'; strncpy(path + 1, fn, 61); path[62] = '\0'; }
        else { strncpy(path, fn, 63); path[63] = '\0'; }
        int pl = (int)strlen(path);
        while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';
        if (vfs_read(path, fbuf, (uint32_t)(sizeof(fbuf) - 1), &fsize) != 0) {
            kprintf("tsort: cannot read '%s'\n", fn);
            return 1;
        }
        fbuf[fsize] = '\0';
    } else {
        if (!shell_has_stdin()) {
            kprintf("Usage: tsort [file]\n");
            return 1;
        }
        fsize = (uint32_t)shell_stdin_read(fbuf, (int)sizeof(fbuf) - 1);
        fbuf[fsize] = '\0';
    }

    /* Parse pairs */
    char *p = fbuf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') { p++; continue; }
        if (*p == '\0') break;

        char word1[64], word2[64];
        int w1 = 0, w2 = 0;

        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && w1 < 63)
            word1[w1++] = *p++;
        word1[w1] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && w2 < 63)
            word2[w2++] = *p++;
        word2[w2] = '\0';

        if (w1 == 0) break;
        if (w2 == 0) {
            /* Single node with no dependency */
            int id = find_node(nodes, &ncount, word1);
            if (id < 0) { kprintf("tsort: too many nodes\n"); return 1; }
            break;
        }

        int id1 = find_node(nodes, &ncount, word1);
        int id2 = find_node(nodes, &ncount, word2);
        if (id1 < 0 || id2 < 0) {
            kprintf("tsort: too many nodes\n");
            return 1;
        }

        if (ecount >= MAX_EDGES) {
            kprintf("tsort: too many edges\n");
            return 1;
        }
        edges[ecount].from = id1;
        edges[ecount].to = id2;
        nodes[id1].outdegree++;
        nodes[id2].indegree++;
        ecount++;
    }

    /* Kahn's algorithm */
    static int queue[MAX_NODES];
    int qhead = 0, qtail = 0;

    /* Enqueue all nodes with indegree 0 */
    for (int i = 0; i < ncount; i++) {
        if (nodes[i].indegree == 0)
            queue[qtail++] = i;
    }

    int processed = 0;
    while (qhead < qtail) {
        int nid = queue[qhead++];
        kprintf("%s\n", nodes[nid].name);
        processed++;

        /* Decrement indegree of all successors */
        for (int e = 0; e < ecount; e++) {
            if (edges[e].from == nid) {
                int to = edges[e].to;
                nodes[to].indegree--;
                if (nodes[to].indegree == 0)
                    queue[qtail++] = to;
            }
        }
    }

    if (processed < ncount) {
        kprintf("tsort: warning: cycle detected, remaining nodes:\n");
        for (int i = 0; i < ncount; i++) {
            if (nodes[i].indegree > 0)
                kprintf("  %s\n", nodes[i].name);
        }
        return 1;
    }
    return 0;
}
