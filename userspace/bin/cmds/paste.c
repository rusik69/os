/* paste.c — merge lines of files side by side */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define MAX_FILES 64
#define MAX_LINE_LEN 4096

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: paste FILE...\n");
        return 1;
    }
    int num_files = argc - 1;
    if (num_files > MAX_FILES) num_files = MAX_FILES;
    int fds[MAX_FILES];
    int open_count = 0;
    for (int i = 0; i < num_files; i++) {
        fds[i] = open(argv[i + 1], O_RDONLY, 0);
        if (fds[i] >= 0) open_count++;
    }
    if (open_count == 0) {
        printf("paste: no valid files\n");
        return 1;
    }
    /* Read lines from each file in parallel, output tab-separated */
    int done[MAX_FILES];
    memset(done, 0, sizeof(done));
    int any_alive = 1;
    while (any_alive) {
        any_alive = 0;
        for (int i = 0; i < num_files; i++) {
            if (done[i]) continue;
            if (fds[i] < 0) { done[i] = 1; continue; }
            char line[MAX_LINE_LEN];
            int pos = 0;
            char ch;
            while (read(fds[i], &ch, 1) == 1) {
                any_alive = 1;
                if (ch == '\n') break;
                if (pos < MAX_LINE_LEN - 1) line[pos++] = ch;
            }
            line[pos] = '\0';
            if (i > 0) write(STDOUT_FILENO, "\t", 1);
            write(STDOUT_FILENO, line, pos);
            if (pos == 0 && ch != '\n') done[i] = 1;
        }
        write(STDOUT_FILENO, "\n", 1);
    }
    for (int i = 0; i < num_files; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
    return 0;
}
