/* zgrep.c — Grep compressed files (pipe through decompress then grep) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static int is_gzipped(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return 0;
    unsigned char magic[2];
    int n = read(fd, magic, 2);
    close(fd);
    return (n == 2 && magic[0] == 0x1f && magic[1] == 0x8b);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: zgrep <pattern> <file...>\n");
        return 1;
    }

    const char *pattern = argv[1];

    for (int i = 2; i < argc; i++) {
        int gz = is_gzipped(argv[i]);

        if (!gz) {
            int pid = fork();
            if (pid == 0) {
                char *gr_argv[] = { (char *)"grep", (char *)"-H", (char *)pattern, (char *)argv[i], NULL };
                char *env[] = { NULL, NULL };
                execve("/bin/grep", gr_argv, env);
                printf("zgrep: grep not found\n");
                exit(1);
            }
            int status;
            waitpid(pid, &status, 0);
        } else {
            int pipe_fds[2];
            pipe(pipe_fds);

            int pid = fork();
            if (pid == 0) {
                close(pipe_fds[0]);
                dup2(pipe_fds[1], 1);
                close(pipe_fds[1]);
                char *gz_argv[] = { (char *)"gunzip", (char *)"-c", (char *)argv[i], NULL };
                char *env[] = { NULL, NULL };
                execve("/bin/gunzip", gz_argv, env);
                /* If gunzip not found, read file directly */
                int fd = open(argv[i], O_RDONLY, 0);
                if (fd >= 0) {
                    char buf[8192];
                    int n;
                    while ((n = read(fd, buf, sizeof(buf))) > 0)
                        write(1, buf, n);
                    close(fd);
                }
                exit(1);
            }

            int pid2 = fork();
            if (pid2 == 0) {
                close(pipe_fds[1]);
                dup2(pipe_fds[0], 0);
                close(pipe_fds[0]);
                char *gr_argv2[] = { (char *)"grep", (char *)"-H", (char *)pattern, NULL };
                char *env2[] = { NULL, NULL };
                execve("/bin/grep", gr_argv2, env2);
                /* Fallback grep inline */
                char line[4096];
                const char *label = argv[i];
                while (1) {
                    int pos = 0;
                    char c;
                    while (pos < (int)sizeof(line) - 1) {
                        if (read(0, &c, 1) <= 0) break;
                        if (c == '\n') break;
                        line[pos++] = c;
                    }
                    line[pos] = 0;
                    if (pos == 0) {
                        char tmp[2];
                        if (read(0, tmp, 1) <= 0) break;
                    }
                    if (strstr(line, pattern)) {
                        printf("%s: %s\n", label, line);
                    }
                }
                exit(0);
            }

            close(pipe_fds[0]);
            close(pipe_fds[1]);
            int status;
            waitpid(pid, &status, 0);
            waitpid(pid2, &status, 0);
        }
    }

    return 0;
}
