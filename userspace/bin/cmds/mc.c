/* mc.c — simple file manager listing directory contents with colors */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define BUF_SIZE 8192

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : ".";

    /* Print header */
    write(1, "\033[1m", 4);  /* bold */
    write(1, " -- File Manager -- ", 20);
    write(1, "\033[0m", 4);
    write(1, "\n", 1);
    write(1, "\033[44;37m", 8);  /* blue bg */
    write(1, " Path: ", 7);
    write(1, path, strlen(path));
    write(1, " \033[0m", 5);
    write(1, "\n\n", 2);

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("mc: cannot open '%s'\n", path);
        return 1;
    }

    char buf[BUF_SIZE];
    int n = getdents64(fd, buf, BUF_SIZE);
    close(fd);

    if (n < 0) {
        printf("mc: getdents failed\n");
        return 1;
    }

    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        const char *name = d->d_name;

        /* Skip . and .. */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            pos += d->d_reclen;
            continue;
        }

        /* Get file info to determine type and permissions */
        struct stat st;
        int is_dir = 0;
        int is_exec = 0;

        /* Build full path for stat */
        char fullpath[1024];
        if (path[0] == '/' && path[1] == 0) {
            int len = strlen(name);
            if (len > 1020) len = 1020;
            memcpy(fullpath, path, 1);
            memcpy(fullpath + 1, name, len);
            fullpath[1 + len] = 0;
        } else {
            int plen = strlen(path);
            int nlen = strlen(name);
            if (plen + 1 + nlen > 1020) nlen = 1020 - plen - 1;
            memcpy(fullpath, path, plen);
            fullpath[plen] = '/';
            memcpy(fullpath + plen + 1, name, nlen);
            fullpath[plen + 1 + nlen] = 0;
        }

        if (stat(fullpath, &st) == 0) {
            if ((st.st_mode & 0170000) == 0040000)
                is_dir = 1;
            else if (st.st_mode & 00111)
                is_exec = 1;
        } else {
            /* Use d_type from dirent as fallback */
            if (d->d_type == 4)  /* DT_DIR */
                is_dir = 1;
        }

        /* Entry prefix */
        write(1, " ", 1);
        if (is_dir) {
            write(1, "\033[34m", 5);  /* blue */
            write(1, "d", 1);
        } else if (is_exec) {
            write(1, "\033[32m", 5);  /* green */
            write(1, "f", 1);
        } else {
            write(1, "\033[37m", 5);  /* white */
            write(1, "f", 1);
        }
        write(1, "\033[0m", 4);

        write(1, "  ", 2);

        /* Entry name with color */
        if (is_dir) {
            write(1, "\033[34m", 5);  /* blue */
            write(1, name, strlen(name));
            write(1, "/", 1);
            write(1, "\033[0m", 4);
        } else if (is_exec) {
            write(1, "\033[32m", 5);  /* green */
            write(1, name, strlen(name));
            write(1, "\033[0m", 4);
            write(1, "*", 1);
        } else {
            write(1, name, strlen(name));
        }

        /* Show size for files */
        if (!is_dir && stat(fullpath, &st) == 0) {
            write(1, "  ", 2);
            unsigned long long sz = st.st_size;
            char szbuf[32];
            int szpos = 30;
            szbuf[31] = 0;
            if (sz == 0) {
                memcpy(szbuf + 30, "0", 1);
                szpos = 30;
            } else {
                while (sz > 0 && szpos > 0) {
                    szpos--;
                    szbuf[szpos] = '0' + (sz % 10);
                    sz /= 10;
                }
            }
            write(1, "(", 1);
            write(1, szbuf + szpos, 31 - szpos);
            write(1, ")", 1);
        }

        write(1, "\n", 1);
        pos += d->d_reclen;
    }

    write(1, "\n", 1);
    return 0;
}
