/*
 * readdir.c — getdents64 syscall for directory traversal
 *
 * Implements the getdents64 syscall (SYS_GETDENTS64 = 78 on x86_64),
 * which reads directory entries into a userspace buffer using the
 * Linux struct linux_dirent64 format.
 *
 * The syscall fills a buffer with one or more struct linux_dirent64
 * entries and returns the number of bytes read.  When the directory
 * has no more entries, it returns 0.
 *
 * The implementation uses the existing vfs_readdir_names() VFS API to
 * enumerate directory entries by path.
 *
 * Item 453: getdents64 syscall
 */

#define KERNEL_INTERNAL
#include "vfs.h"
#include "fs.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "syscall.h"
#include "export.h"
#include "process.h"

/* ── POSIX file type macros (stat.h equivalent) ────────────────────── */
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & 0170000) == 0100000)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & 0170000) == 0040000)
#endif
#ifndef S_ISCHR
#define S_ISCHR(m)  (((m) & 0170000) == 0020000)
#endif
#ifndef S_ISBLK
#define S_ISBLK(m)  (((m) & 0170000) == 0060000)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(m) (((m) & 0170000) == 0010000)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & 0170000) == 0140000)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (((m) & 0170000) == 0120000)
#endif

/* ── File type constants (matches Linux DT_*) ──────────────────── */
#define DT_UNKNOWN  0
#define DT_REG      1
#define DT_DIR      2
#define DT_CHR      3
#define DT_BLK      4
#define DT_FIFO     5
#define DT_SOCK     6
/* DT_LNK is defined in syscall.h */

/* ── VFS type mapping helper ───────────────────────────────────────── */

/* Map a path to a d_type by stat-ing it. */
static unsigned char path_to_dt(const char *path)
{
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0)
        return DT_UNKNOWN;

    uint32_t mode = st.mode;
    if (S_ISREG(mode))  return DT_REG;
    if (S_ISDIR(mode))  return DT_DIR;
    if (S_ISCHR(mode))  return DT_CHR;
    if (S_ISBLK(mode))  return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISSOCK(mode)) return DT_SOCK;
    if (S_ISLNK(mode))  return DT_LNK;
    return DT_UNKNOWN;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  getdents64 implementation
 * ═══════════════════════════════════════════════════════════════════════ */

/* Fill a buffer with directory entries for a given directory path.
 *
 * @dir_path: path to the directory (e.g., "/tmp" or "/")
 * @dirp:     userspace buffer pointer
 * @count:    size of the buffer in bytes
 * @cookie:   offset into directory (for resumption, 0 = start)
 *
 * Returns number of bytes written to dirp on success, 0 at end of
 * directory, or negative errno on error.
 */
int64_t sys_getdents64(const char *dir_path, void *dirp, uint64_t count,
                       uint64_t cookie)
{
    if (!dir_path || !dirp || count < sizeof(struct linux_dirent64))
        return -EINVAL;

    /* Fetch all directory entries via the VFS API */
    char names[64][64];
    int num_entries = vfs_readdir_names(dir_path, names, 64);
    if (num_entries < 0)
        return num_entries;

    /* Skip entries before cookie */
    int start_idx = (int)cookie;
    if (start_idx < 0) start_idx = 0;
    if (start_idx >= num_entries)
        return 0; /* end of directory */

    /* Serialize entries into the Linux dirent64 format */
    uint64_t buf_pos = 0;
    uint8_t *buf = (uint8_t *)dirp;

    for (int i = start_idx; i < num_entries; i++) {
        const char *name = names[i];
        size_t name_len = strlen(name);
        if (name_len > 255) name_len = 255;

        /* Calculate record length (aligned to 8 bytes) */
        unsigned short reclen = (unsigned short)
            (sizeof(struct linux_dirent64) + name_len + 1);
        reclen = (unsigned short)((reclen + 7) & ~7);

        if (buf_pos + reclen > count)
            break;

        struct linux_dirent64 *entry = (struct linux_dirent64 *)(buf + buf_pos);
        entry->d_ino = 0;  /* inode number not exposed by VFS */
        entry->d_off = (int64_t)(i + 1);  /* next cookie */
        entry->d_reclen = reclen;

        /* Determine file type by stat-ing the full path */
        char full_path[256];
        size_t dir_len = strlen(dir_path);
        if (dir_len + 1 + name_len + 1 <= sizeof(full_path)) {
            memcpy(full_path, dir_path, dir_len);
            if (full_path[dir_len - 1] != '/') {
                full_path[dir_len] = '/';
                dir_len++;
            }
            memcpy(full_path + dir_len, name, name_len + 1);
            entry->d_type = path_to_dt(full_path);
        } else {
            entry->d_type = DT_UNKNOWN;
        }

        memcpy(entry->d_name, name, name_len);
        entry->d_name[name_len] = '\0';

        buf_pos += reclen;
    }

    return (int64_t)buf_pos;
}

/* Syscall handler wrapper for getdents64.
 * This is called from the syscall dispatcher with:
 *   fd:    file descriptor of an open directory
 *   dirp:  destination buffer
 *   count: buffer size
 *
 * In this kernel, the VFS uses path-based lookups internally.
 * The fd- or path-based variant can be selected by the caller.
 */
int64_t handle_getdents64(int fd, void *dirp, uint64_t count)
{
    /* Look up the path from the current process's fd table */
    struct process *cur = process_get_current();
    if (!cur || fd < 0 || fd >= PROCESS_FD_MAX)
        return -EBADF;

    struct process_fd *pfd = &cur->fd_table[fd];
    if (!pfd->used)
        return -EBADF;

    /* The fd's path may include the offset cookie; handle_getdents64_path
     * calls sys_getdents64 which reads entries by path. */
    const char *path = pfd->path;
    return sys_getdents64(path, dirp, count, 0);
}

/* Path-based getdents64: takes a directory path and cookie for resumption.
 * This is the primary entry point. */
int64_t handle_getdents64_path(const char *path, void *dirp,
                                uint64_t count, uint64_t cookie)
{
    return sys_getdents64(path, dirp, count, cookie);
}
