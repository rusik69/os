/* realpath.c — print resolved absolute path */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

#define REALPATH_MAX 4096

/* Resolve a single path to an absolute path.
 * This implements . and .. resolution and symlink following.
 * Returns 0 on success, -1 on failure. */
static int do_realpath(const char *path, char *out, unsigned long out_size) {
    char tmp[REALPATH_MAX];
    char result[REALPATH_MAX];
    char buf[REALPATH_MAX];

    /* If path is relative, prepend cwd */
    if (path[0] != '/') {
        if (getcwd(tmp, REALPATH_MAX) < 0) {
            return -1;
        }
        unsigned long cwd_len = strlen(tmp);
        if (cwd_len + 1 + strlen(path) >= REALPATH_MAX) return -1;
        tmp[cwd_len] = '/';
        strcpy(tmp + cwd_len + 1, path);
    } else {
        strncpy(tmp, path, REALPATH_MAX - 1);
        tmp[REALPATH_MAX - 1] = 0;
    }

    /* Normalize: resolve ., .., and multiple slashes */
    char components[256][256];  /* max 256 path components, 256 chars each */
    int ncomp = 0;

    char *p = tmp;
    char *slash;

    while (*p) {
        /* Skip leading slashes */
        while (*p == '/') p++;
        if (*p == 0) break;

        /* Find end of this component */
        slash = p;
        while (*slash && *slash != '/') slash++;
        char comp[256];
        unsigned long clen = (unsigned long)(slash - p);
        if (clen >= 256) return -1;
        memcpy(comp, p, clen);
        comp[clen] = 0;

        if (strcmp(comp, ".") == 0) {
            /* Skip */
        } else if (strcmp(comp, "..") == 0) {
            if (ncomp > 0) ncomp--;
        } else {
            /* Check if this component is a symlink */
            /* Build path so far */
            char full[REALPATH_MAX];
            unsigned long pos = 0;
            full[pos++] = '/';
            for (int ci = 0; ci < ncomp; ci++) {
                unsigned long len = strlen(components[ci]);
                if (pos + len + 1 >= REALPATH_MAX) return -1;
                memcpy(full + pos, components[ci], len);
                pos += len;
                full[pos++] = '/';
            }
            unsigned long clen2 = (unsigned long)(slash - p);
            if (pos + clen2 >= REALPATH_MAX) return -1;
            memcpy(full + pos, comp, clen2);
            pos += clen2;
            full[pos] = 0;

            /* stat to check if symlink */
            if (readlink(full, buf, REALPATH_MAX) > 0) {
                /* It's a symlink — read target and re-process */
                int rl = readlink(full, buf, REALPATH_MAX - 1);
                if (rl < 0) return -1;
                buf[rl] = 0;

                /* Prepend remaining path */
                char remaining[REALPATH_MAX];
                remaining[0] = 0;
                if (*slash) {
                    strncpy(remaining, slash, REALPATH_MAX - 1);
                    remaining[REALPATH_MAX - 1] = 0;
                }

                /* Construct new path from symlink target + remaining */
                char newpath[REALPATH_MAX];
                if (buf[0] == '/') {
                    /* Absolute symlink target */
                    snprintf(newpath, REALPATH_MAX, "%s%s", buf, remaining);
                } else {
                    /* Relative symlink target — prepend directory of current path */
                    /* Build parent directory of full */
                    char parent[REALPATH_MAX];
                    strncpy(parent, full, REALPATH_MAX - 1);
                    parent[REALPATH_MAX - 1] = 0;
                    char *last_slash = strrchr(parent, '/');
                    if (last_slash && last_slash != parent) *last_slash = 0;
                    else parent[1] = 0;

                    snprintf(newpath, REALPATH_MAX, "%s/%s%s", parent, buf, remaining);
                }

                /* Recursively resolve the new path */
                return do_realpath(newpath, out, out_size);
            }

            /* Regular component — add to list */
            strncpy(components[ncomp], comp, 256);
            components[ncomp][255] = 0;
            ncomp++;
        }

        p = slash;
        if (*slash == '/') p = slash + 1;
    }

    /* Build result */
    unsigned long pos = 0;
    result[pos++] = '/';
    for (int ci = 0; ci < ncomp; ci++) {
        if (ci > 0) result[pos++] = '/';
        unsigned long len = strlen(components[ci]);
        if (pos + len >= REALPATH_MAX) return -1;
        memcpy(result + pos, components[ci], len);
        pos += len;
    }
    result[pos] = 0;

    if (strlen(result) >= out_size) return -1;
    strcpy(out, result);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: realpath <path...>\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        char resolved[REALPATH_MAX];
        if (do_realpath(argv[i], resolved, REALPATH_MAX) == 0) {
            printf("%s\n", resolved);
        } else {
            printf("realpath: cannot resolve '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
