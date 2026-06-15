/* zdiff.c — Diff compressed files (pipe through decompress then diff) */
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
        printf("usage: zdiff <file1> <file2>\n");
        return 1;
    }

    int gz1 = is_gzipped(argv[1]);
    int gz2 = is_gzipped(argv[2]);

    if (!gz1 && !gz2) {
        char *diff_argv[4] = { "diff", argv[1], argv[2], NULL };
        execve("/bin/diff", diff_argv, (char *const[]){ NULL });
        printf("zdiff: diff not found\n");
        return 1;
    }

    char tmp1[] = "/tmp/zdiff1XXXXXX";
    char tmp2[] = "/tmp/zdiff2XXXXXX";

    if (gz1) {
        int pid = fork();
        if (pid == 0) {
            int fd = open(tmp1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) exit(1);
            dup2(fd, 1);
            close(fd);
            char *gz_argv[4] = { "gunzip", "-c", argv[1], NULL };
            execve("/bin/gunzip", gz_argv, (char *const[]){ NULL });
            exit(1);
        }
        int status;
        waitpid(pid, &status, 0);
    } else {
        int infd = open(argv[1], O_RDONLY, 0);
        int outfd = open(tmp1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (infd >= 0 && outfd >= 0) {
            char buf[8192];
            int n;
            while ((n = read(infd, buf, sizeof(buf))) > 0)
                write(outfd, buf, n);
            close(infd);
            close(outfd);
        }
    }

    if (gz2) {
        int pid = fork();
        if (pid == 0) {
            int fd = open(tmp2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) exit(1);
            dup2(fd, 1);
            close(fd);
            char *gz_argv[4] = { "gunzip", "-c", argv[2], NULL };
            execve("/bin/gunzip", gz_argv, (char *const[]){ NULL });
            exit(1);
        }
        int status;
        waitpid(pid, &status, 0);
    } else {
        int infd = open(argv[2], O_RDONLY, 0);
        int outfd = open(tmp2, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (infd >= 0 && outfd >= 0) {
            char buf[8192];
            int n;
            while ((n = read(infd, buf, sizeof(buf))) > 0)
                write(outfd, buf, n);
            close(infd);
            close(outfd);
        }
    }

    char *diff_argv[4] = { "diff", tmp1, tmp2, NULL };
    int pid = fork();
    if (pid == 0) {
        execve("/bin/diff", diff_argv, (char *const[]){ NULL });
        printf("zdiff: diff not found\n");
        exit(1);
    }
    int status;
    waitpid(pid, &status, 0);

    unlink(tmp1);
    unlink(tmp2);

    return status;
}
