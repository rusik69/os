#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include "unistd.h"

/* File type (st_mode bits) */
#define S_IFMT   0170000   /* bit mask for the file type bit field */
#define S_IFSOCK 0140000   /* socket */
#define S_IFLNK  0120000   /* symbolic link */
#define S_IFREG  0100000   /* regular file */
#define S_IFBLK  0060000   /* block device */
#define S_IFDIR  0040000   /* directory */
#define S_IFCHR  0020000   /* character device */
#define S_IFIFO  0010000   /* FIFO */

/* File mode bits */
#define S_ISUID  0004000   /* set user id on execution */
#define S_ISGID  0002000   /* set group id on execution */
#define S_ISVTX  0001000   /* sticky bit */

#define S_IRWXU  00700     /* owner read/write/execute */
#define S_IRUSR  00400     /* owner read */
#define S_IWUSR  00200     /* owner write */
#define S_IXUSR  00100     /* owner execute */

#define S_IRWXG  00070     /* group read/write/execute */
#define S_IRGRP  00040     /* group read */
#define S_IWGRP  00020     /* group write */
#define S_IXGRP  00010     /* group execute */

#define S_IRWXO  00007     /* other read/write/execute */
#define S_IROTH  00004     /* other read */
#define S_IWOTH  00002     /* other write */
#define S_IXOTH  00001     /* other execute */

/* Macros for checking file type */
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Function prototype */
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, int mode);

#endif /* _SYS_STAT_H */
