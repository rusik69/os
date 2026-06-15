/* lsattr.c — list file attributes (permissions/mode) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

#define S_IFMT   0170000
#define S_IFBLK  0060000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFIFO  0010000
#define S_IFLNK  0120000
#define S_IFSOCK 0140000

#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_IRUSR  00400
#define S_IWUSR  00200
#define S_IXUSR  00100
#define S_IRGRP  00040
#define S_IWGRP  00020
#define S_IXGRP  00010
#define S_IROTH  00004
#define S_IWOTH  00002
#define S_IXOTH  00001

#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

static void mode_to_str(unsigned int mode, char *buf) {
    /* File type */
    if (S_ISREG(mode))  buf[0] = '-';
    else if (S_ISDIR(mode))  buf[0] = 'd';
    else if (S_ISCHR(mode))  buf[0] = 'c';
    else if (S_ISBLK(mode))  buf[0] = 'b';
    else if (S_ISFIFO(mode)) buf[0] = 'p';
    else if (S_ISLNK(mode))  buf[0] = 'l';
    else if (S_ISSOCK(mode)) buf[0] = 's';
    else buf[0] = '?';

    /* Owner */
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    if (mode & S_IXUSR)
        buf[3] = (mode & S_ISUID) ? 's' : 'x';
    else
        buf[3] = (mode & S_ISUID) ? 'S' : '-';

    /* Group */
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    if (mode & S_IXGRP)
        buf[6] = (mode & S_ISGID) ? 's' : 'x';
    else
        buf[6] = (mode & S_ISGID) ? 'S' : '-';

    /* Other */
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    if (mode & S_IXOTH)
        buf[9] = (mode & S_ISVTX) ? 't' : 'x';
    else
        buf[9] = (mode & S_ISVTX) ? 'T' : '-';

    buf[10] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: lsattr <file...>\n");
        return 1;
    }

    int ret = 0;

    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            printf("lsattr: cannot stat '%s'\n", argv[i]);
            ret = 1;
            continue;
        }

        char modestr[11];
        mode_to_str(st.st_mode, modestr);

        printf("lsattr %s  %s  size=%llu  uid=%u  gid=%u\n",
               argv[i], modestr,
               (unsigned long long)st.st_size,
               st.st_uid, st.st_gid);
    }

    return ret;
}
