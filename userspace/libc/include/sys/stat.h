/*
 * sys/stat.h — File type and mode constants for the stat() family of calls
 *
 * This header defines the st_mode bit layout used by stat(), fstat(), and
 * related filesystem calls.  The mode word (mode_t) is split into three
 * logical groups, from most-significant to least-significant bits:
 *
 *   Bits 15-12  — File type (S_IFMT mask, octal 0170000)
 *   Bits 11-9   — Special mode bits (setuid / setgid / sticky)
 *   Bits 8-0    — Permission bits (owner / group / other, rwx each)
 *
 * Permission bits follow the traditional Unix 3×3 model: owner, group, and
 * other each have separate read (r), write (w), and execute (x) bits.
 *
 * The S_IS*(m) convenience macros let callers test the file type without
 * manually masking against S_IFMT.
 *
 * Reference: POSIX.1-2017 <sys/stat.h>, sections "stat", "fstat", "mkdir",
 * and the S_IRWXU / S_ISUID / S_IFMT families of constants.
 */

#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include "unistd.h"

/*
 * File-type constants (st_mode bits 15-12, masked by S_IFMT)
 *
 * These values occupy the top four bits of the mode word.  Exactly one
 * type should be set in any valid inode.  The numeric values are chosen
 * to match historical Unix (AT&T SVID / POSIX) conventions.
 *
 *   Octal   Binary (bits 15-12)   Meaning
 *   ─────   ───────────────────   ───────
 *   014     1100                  Socket (S_IFSOCK)
 *   012     1010                  Symbolic link (S_IFLNK)
 *   010     1000                  Regular file (S_IFREG)
 *   006     0110                  Block device (S_IFBLK)
 *   004     0100                  Directory (S_IFDIR)
 *   002     0010                  Character device (S_IFCHR)
 *   001     0001                  FIFO / named pipe (S_IFIFO)
 */
#define S_IFMT   0170000   /* bit mask for the file type bit field */
#define S_IFSOCK 0140000   /* socket */
#define S_IFLNK  0120000   /* symbolic link */
#define S_IFREG  0100000   /* regular file */
#define S_IFBLK  0060000   /* block device */
#define S_IFDIR  0040000   /* directory */
#define S_IFCHR  0020000   /* character device */
#define S_IFIFO  0010000   /* FIFO */

/*
 * Special mode bits (st_mode bits 11-9)
 *
 * These modify execution semantics for regular files and directories.
 *
 *   S_ISUID (04000) — set-user-ID on execution: the effective UID of the
 *                     running process is set to the file owner's UID.
 *   S_ISGID (02000) — set-group-ID on execution: the effective GID is set
 *                     to the file group's GID.  On directories, newly
 *                     created children inherit the directory's GID.
 *   S_ISVTX (01000) — sticky bit: on directories, prevents non-owners from
 *                     deleting or renaming files they do not own (e.g. /tmp).
 *                     Historically kept the text segment in swap.
 */
#define S_ISUID  0004000   /* set user id on execution */
#define S_ISGID  0002000   /* set group id on execution */
#define S_ISVTX  0001000   /* sticky bit */

/*
 * Owner permission bits (bits 8-6)
 */
#define S_IRWXU  00700     /* owner read/write/execute */
#define S_IRUSR  00400     /* owner read */
#define S_IWUSR  00200     /* owner write */
#define S_IXUSR  00100     /* owner execute */

/*
 * Group permission bits (bits 5-3)
 */
#define S_IRWXG  00070     /* group read/write/execute */
#define S_IRGRP  00040     /* group read */
#define S_IWGRP  00020     /* group write */
#define S_IXGRP  00010     /* group execute */

/*
 * Other (world) permission bits (bits 2-0)
 */
#define S_IRWXO  00007     /* other read/write/execute */
#define S_IROTH  00004     /* other read */
#define S_IWOTH  00002     /* other write */
#define S_IXOTH  00001     /* other execute */

/*
 * File-type test macros
 *
 * Each macro evaluates to non-zero if the file type extracted from the mode
 * matches the given type.  Typical usage:
 *
 *   struct stat st;
 *   stat("/path/to/file", &st);
 *   if (S_ISREG(st.st_mode)) { ... }
 */
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
