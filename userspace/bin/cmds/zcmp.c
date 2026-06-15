/* zcmp.c — Compare compressed files (pipe through decompress then cmp) */
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
        printf("usage: zcmp <file1> <file2>\n");
        return 1;
    }

    /* Check if files are compressed */
    int gz1 = is_gzipped(argv[1]);
    int gz2 = is_gzipped(argv[2]);

    if (!gz1 && !gz2) {
        /* Pass through to cmp directly */
        char *cmp_argv[4] = { "cmp", argv[1], argv[2], NULL };
        execve("/bin/cmp", cmp_argv, (char *const[]){ NULL });
        printf("zcmp: cmp not found\n");
        return 1;
    }

    /* Decompress to temp files */
    char tmp1[] = "/tmp/zcmp1XXXXXX";
    char tmp2[] = "/tmp/zcmp2XXXXXX";

    /* Decompress first file */
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

    /* Decompress second file */
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

    /* Now run cmp on the temp files */
    char *cmp_argv[4] = { "cmp", tmp1, tmp2, NULL };
    int pid = fork();
    if (pid == 0) {
        execve("/bin/cmp", cmp_argv, (char *const[]){ NULL });
        printf("zcmp: cmp not found\n");
        exit(1);
    }
    int status;
    waitpid(pid, &status, 0);

    /* Cleanup */
    unlink(tmp1);
    unlink(tmp2);

    return status;
}
