/* stat.c — display file status via stat() */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "sys/stat.h"

static void print_mode(unsigned int mode) {
    /* File type */
    if (S_ISREG(mode)) write(STDOUT_FILENO, "-", 1);
    else if (S_ISDIR(mode)) write(STDOUT_FILENO, "d", 1);
    else if (S_ISCHR(mode)) write(STDOUT_FILENO, "c", 1);
    else if (S_ISBLK(mode)) write(STDOUT_FILENO, "b", 1);
    else if (S_ISFIFO(mode)) write(STDOUT_FILENO, "p", 1);
    else if (S_ISLNK(mode)) write(STDOUT_FILENO, "l", 1);
    else if (S_ISSOCK(mode)) write(STDOUT_FILENO, "s", 1);
    else write(STDOUT_FILENO, "?", 1);
    /* Owner permissions */
    write(STDOUT_FILENO, (mode & S_IRUSR) ? "r" : "-", 1);
    write(STDOUT_FILENO, (mode & S_IWUSR) ? "w" : "-", 1);
    write(STDOUT_FILENO, (mode & S_IXUSR) ? "x" : "-", 1);
    /* Group permissions */
    write(STDOUT_FILENO, (mode & S_IRGRP) ? "r" : "-", 1);
    write(STDOUT_FILENO, (mode & S_IWGRP) ? "w" : "-", 1);
    write(STDOUT_FILENO, (mode & S_IXGRP) ? "x" : "-", 1);
    /* Other permissions */
    write(STDOUT_FILENO, (mode & S_IROTH) ? "r" : "-", 1);
    write(STDOUT_FILENO, (mode & S_IWOTH) ? "w" : "-", 1);
    write(STDOUT_FILENO, (mode & S_IXOTH) ? "x" : "-", 1);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: stat FILE...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("stat: cannot stat '%s'\n", argv[i]);
            continue;
        }
        printf("  File: %s\n", argv[i]);
        printf("  Size: %lld", st.st_size);
        printf("  Blocks: %lld", st.st_blocks);
        printf("  IO Block: %lld", st.st_blksize);
        printf("\n");
        printf("  Device: %llx", st.st_dev);
        printf("  Inode: %llu", st.st_ino);
        printf("  Links: %u", st.st_nlink);
        printf("\n");
        printf("  Access: ");
        print_mode(st.st_mode);
        printf("  Uid: %u", st.st_uid);
        printf("  Gid: %u", st.st_gid);
        printf("\n");
        if (i < argc - 1) printf("\n");
    }
    return 0;
}
