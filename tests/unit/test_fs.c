/*
 * test_fs.c — Host-side unit tests for filesystem operations
 *
 * Tests core filesystem logic: inode-based tree operations, path
 * resolution, file create/read/write/delete, directory
 * create/delete, and path edge cases (root, dot, dotdot,
 * trailing slash).
 *
 * The test implements a simplified inode-based filesystem (similar
 * in design to the kernel's tmpfs) so that the algorithmic
 * correctness of create, unlink, mkdir, rmdir, read, and write
 * can be verified independently on the host.
 *
 * Compile:
 *   gcc -Wall -Werror -g -O0 -o test_fs test_fs.c
 * Run:
 *   ./test_fs
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 *  Kernel-compatible filesystem data structures
 *
 *  Mirrors the design of tmpfs in src/fs/tmpfs.c — flat inode
 *  table with name, type, data buffer, parent pointer.
 * =================================================================== */

#define FS_MAX_INODES  256
#define FS_MAX_NAME    64
#define FS_BLOCK_SIZE  4096

/* Inode types */
#define FS_TYPE_FILE  1
#define FS_TYPE_DIR   2
#define FS_TYPE_LINK  3

/* Mode constants */
#define FS_MODE_FILE  0644
#define FS_MODE_DIR   0755

struct fs_inode {
    int      in_use;
    uint8_t  type;
    char     name[FS_MAX_NAME];
    uint32_t size;
    uint8_t  *data;       /* file content (malloc'd) */
    uint32_t parent;      /* index of parent dir */
};

/* ── Global inode table ─────────────────────────────────────────── */
static struct fs_inode inodes[FS_MAX_INODES];

/* ── Stat counters for test reporting ───────────────────────────── */
static int tests_total = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do {                                          \
    tests_total++;                                                      \
    if (!(cond)) {                                                      \
        fprintf(stderr, "  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg);\
    } else {                                                            \
        tests_passed++;                                                 \
    }                                                                   \
} while (0)

static void test_start(const char *name) {
    printf("  TEST: %-55s ... ", name);
    fflush(stdout);
}

static void test_end(void) {
    printf("%s\n", tests_passed == tests_total ? "PASS" : "FAIL");
}

/* ===================================================================
 *  Filesystem operations — identical algorithm to kernel's tmpfs
 * =================================================================== */

/* Allocate a free inode slot (0 = root, 1..FS_MAX_INODES-1 = free) */
static int alloc_inode(void) {
    for (int i = 1; i < FS_MAX_INODES; i++) {
        if (!inodes[i].in_use) {
            inodes[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

/* Free an inode and its data buffer */
static void free_inode(int idx) {
    if (idx < 0 || idx >= FS_MAX_INODES) return;
    if (inodes[idx].data) free(inodes[idx].data);
    inodes[idx].in_use = 0;
    inodes[idx].data = NULL;
    inodes[idx].size = 0;
}

/* Find inode by absolute path. Walks the tree component by component.
 * Returns index or -1. */
static int find_inode(const char *path) {
    if (!path || path[0] != '/') return -1;
    if (path[1] == '\0') return 0; /* root dir */

    /* Walk the path component by component */
    int current = 0; /* start at root */
    const char *p = path + 1; /* skip leading '/' */
    if (*p == '\0') return current;

    while (*p) {
        /* Skip leading slashes */
        while (*p == '/') p++;
        if (*p == '\0') break;

        /* Find the end of this component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int comp_len = (int)(p - start);

        /* Look for a child of `current` with this name */
        int found = -1;
        for (int i = 0; i < FS_MAX_INODES; i++) {
            if (!inodes[i].in_use) continue;
            if (inodes[i].parent != (uint32_t)current) continue;
            if ((int)strlen(inodes[i].name) == comp_len &&
                memcmp(inodes[i].name, start, (size_t)comp_len) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) return -1; /* component not found */
        current = found;
    }
    return current;
}

/* Find a child entry by name inside a directory inode */
static int find_inode_in_dir(int dir_idx, const char *name) {
    if (dir_idx < 0 || dir_idx >= FS_MAX_INODES) return -1;
    if (!inodes[dir_idx].in_use || inodes[dir_idx].type != FS_TYPE_DIR)
        return -1;

    int len = (int)strlen(name);
    for (int i = 0; i < FS_MAX_INODES; i++) {
        if (!inodes[i].in_use) continue;
        if (inodes[i].parent != (uint32_t)dir_idx) continue;
        if ((int)strlen(inodes[i].name) == len &&
            memcmp(inodes[i].name, name, (size_t)len) == 0)
            return i;
    }
    return -1;
}

/* Count children of a directory inode */
static int count_children(int dir_idx) {
    int count = 0;
    for (int i = 0; i < FS_MAX_INODES; i++) {
        if (inodes[i].in_use && inodes[i].parent == (uint32_t)dir_idx)
            count++;
    }
    return count;
}

/* Mount a fresh filesystem (clears all inodes, creates root) */
static void fs_mount(void) {
    memset(inodes, 0, sizeof(inodes));
    inodes[0].in_use = 1;
    inodes[0].type = FS_TYPE_DIR;
    inodes[0].name[0] = '\0';
    inodes[0].parent = 0;
    inodes[0].size = 0;
    inodes[0].data = NULL;
}

/* Unmount and free all data */
static void fs_unmount(void) {
    for (int i = 0; i < FS_MAX_INODES; i++) {
        if (inodes[i].in_use)
            free_inode(i);
    }
}

/* Create a file or directory at the given path */
static int fs_create(const char *path, uint8_t type) {
    if (find_inode(path) >= 0) return -1; /* already exists */
    if (type == FS_TYPE_DIR) {
        /* mkdir path */
        if (!path || path[0] != '/') return -1;
        const char *name = path + 1;
        if (name[0] == '\0') return -1; /* can't recreate root */

        /* Extract parent dir path and basename */
        char dir[FS_MAX_NAME * 2], basename[FS_MAX_NAME];
        const char *slash = strrchr(path, '/');
        if (!slash) return -1;
        int dirlen = (int)(slash - path);
        if (dirlen == 0) {
            /* Parent is root */
            dir[0] = '/'; dir[1] = '\0';
        } else {
            if (dirlen > (int)sizeof(dir) - 1) return -1;
            memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
        }
        int parent = find_inode(dir);
        if (parent < 0 || inodes[parent].type != FS_TYPE_DIR)
            return -1;

        int len = (int)strlen(slash + 1);
        if (len > FS_MAX_NAME - 1) return -1;
        memcpy(basename, slash + 1, (size_t)len + 1);
        if (find_inode_in_dir(parent, basename) >= 0) return -1;

        int idx = alloc_inode();
        if (idx < 0) return -1;
        inodes[idx].type = FS_TYPE_DIR;
        inodes[idx].parent = (uint32_t)parent;
        memcpy(inodes[idx].name, basename, (size_t)len + 1);
        inodes[idx].size = 0;
        inodes[idx].data = NULL;
        return 0;
    }

    /* File creation */
    if (type != FS_TYPE_FILE) return -1;

    char dir[FS_MAX_NAME * 2], basename[FS_MAX_NAME];
    const char *slash = strrchr(path, '/');
    if (!slash) return -1;
    int dirlen = (int)(slash - path);
    if (dirlen > (int)sizeof(dir) - 1) return -1;
    memcpy(dir, path, (size_t)dirlen); dir[dirlen] = '\0';
    if (dirlen == 0) { dir[0] = '/'; dir[1] = '\0'; }
    int parent = find_inode(dir);
    if (parent < 0) return -1;

    int len = (int)strlen(slash + 1);
    if (len > FS_MAX_NAME - 1) return -1;
    memcpy(basename, slash + 1, (size_t)len + 1);

    int idx = alloc_inode();
    if (idx < 0) return -1;
    inodes[idx].type = FS_TYPE_FILE;
    inodes[idx].parent = (uint32_t)parent;
    memcpy(inodes[idx].name, basename, (size_t)len + 1);
    inodes[idx].size = 0;
    inodes[idx].data = NULL;
    return 0;
}

/* Write data to a file */
static int fs_write(const char *path, const void *buf, uint32_t size) {
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != FS_TYPE_FILE)
        return -1;

    if (inodes[idx].size < size || !inodes[idx].data) {
        uint8_t *new = (uint8_t *)malloc(size < 128 ? 128 : size);
        if (!new) return -1;
        if (inodes[idx].data) free(inodes[idx].data);
        inodes[idx].data = new;
    }
    memcpy(inodes[idx].data, buf, size);
    inodes[idx].size = size;
    return 0;
}

/* Read data from a file */
static int fs_read(const char *path, void *buf, uint32_t max, uint32_t *out) {
    int idx = find_inode(path);
    if (idx < 0 || inodes[idx].type != FS_TYPE_FILE)
        return -1;
    uint32_t copy = inodes[idx].size < max ? inodes[idx].size : max;
    if (copy > 0 && inodes[idx].data)
        memcpy(buf, inodes[idx].data, copy);
    *out = copy;
    return 0;
}

/* Unlink (delete) a file */
static int fs_unlink(const char *path) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (inodes[idx].type == FS_TYPE_DIR) return -1; /* use rmdir */
    free_inode(idx);
    return 0;
}

/* Remove an empty directory */
static int fs_rmdir(const char *path) {
    int idx = find_inode(path);
    if (idx <= 0) return -1;          /* non-existent or root */
    if (inodes[idx].type != FS_TYPE_DIR) return -1;
    if (count_children(idx) > 0) return -1; /* not empty */
    free_inode(idx);
    return 0;
}

/* Truncate a file */
static int fs_truncate(const char *path, uint32_t len) {
    int idx = find_inode(path);
    if (idx < 0) return -1;
    if (len == 0 && inodes[idx].data) {
        free(inodes[idx].data);
        inodes[idx].data = NULL;
        inodes[idx].size = 0;
    } else if (len < inodes[idx].size) {
        inodes[idx].size = len;
    }
    return 0;
}

/* Check if a path exists */
static int fs_exists(const char *path) {
    return find_inode(path) >= 0 ? 1 : 0;
}

/* ===================================================================
 *  Test cases
 * =================================================================== */

static void test_root_exists(void) {
    test_start("root directory exists after mount");
    fs_mount();
    ASSERT(fs_exists("/") != 0, "root should exist");
    int idx = find_inode("/");
    ASSERT(idx == 0, "root should be inode 0");
    ASSERT(inodes[0].type == FS_TYPE_DIR, "root should be directory");
    fs_unmount();
    test_end();
}

static void test_create_file_in_root(void) {
    test_start("create file in root directory");
    fs_mount();
    int ret = fs_create("/test.txt", FS_TYPE_FILE);
    ASSERT(ret == 0, "create should succeed");
    ASSERT(fs_exists("/test.txt") != 0, "file should exist");
    int idx = find_inode("/test.txt");
    ASSERT(idx > 0, "file should have non-root inode");
    ASSERT(inodes[idx].type == FS_TYPE_FILE, "should be file type");
    ASSERT(inodes[idx].parent == 0, "parent should be root");
    ASSERT(strcmp(inodes[idx].name, "test.txt") == 0,
           "filename should match");
    fs_unmount();
    test_end();
}

static void test_create_directory(void) {
    test_start("create directory");
    fs_mount();
    int ret = fs_create("/mydir", FS_TYPE_DIR);
    ASSERT(ret == 0, "mkdir should succeed");
    ASSERT(fs_exists("/mydir") != 0, "dir should exist");
    int idx = find_inode("/mydir");
    ASSERT(idx > 0, "dir should have non-root inode");
    ASSERT(inodes[idx].type == FS_TYPE_DIR, "should be dir type");
    ASSERT(inodes[idx].parent == 0, "parent should be root");
    fs_unmount();
    test_end();
}

static void test_create_in_subdirectory(void) {
    test_start("create file in subdirectory");
    fs_mount();
    ASSERT(fs_create("/subdir", FS_TYPE_DIR) == 0, "create subdir");
    ASSERT(fs_create("/subdir/hello.txt", FS_TYPE_FILE) == 0,
           "create file in subdir");
    ASSERT(fs_exists("/subdir/hello.txt") != 0, "file should exist");
    int idx = find_inode("/subdir/hello.txt");
    ASSERT(idx > 0, "file should exist");
    int parent = (int)inodes[idx].parent;
    ASSERT(parent == find_inode("/subdir"), "parent should be subdir");
    ASSERT(strcmp(inodes[idx].name, "hello.txt") == 0, "name check");
    fs_unmount();
    test_end();
}

static void test_create_duplicate_fails(void) {
    test_start("create duplicate file fails");
    fs_mount();
    ASSERT(fs_create("/dup.txt", FS_TYPE_FILE) == 0, "first create");
    ASSERT(fs_create("/dup.txt", FS_TYPE_FILE) != 0,
           "duplicate should fail");
    fs_unmount();
    test_end();
}

static void test_create_duplicate_dir_fails(void) {
    test_start("create duplicate directory fails");
    fs_mount();
    ASSERT(fs_create("/dupdir", FS_TYPE_DIR) == 0, "first mkdir");
    ASSERT(fs_create("/dupdir", FS_TYPE_DIR) != 0,
           "duplicate mkdir should fail");
    fs_unmount();
    test_end();
}

static void test_write_and_read_file(void) {
    test_start("write and read file content");
    fs_mount();
    ASSERT(fs_create("/data.txt", FS_TYPE_FILE) == 0, "create");

    const char *content = "Hello, filesystem!";
    size_t len = strlen(content) + 1; /* include null terminator */
    ASSERT(fs_write("/data.txt", content, (uint32_t)len) == 0, "write");

    char buf[128];
    uint32_t out_len = 0;
    ASSERT(fs_read("/data.txt", buf, sizeof(buf), &out_len) == 0,
           "read");
    ASSERT(out_len == (uint32_t)len, "read length match");
    ASSERT(memcmp(buf, content, len) == 0, "content match");

    /* Verify bytes at inode */
    int idx = find_inode("/data.txt");
    ASSERT(inodes[idx].size == (uint32_t)len, "stored size match");
    ASSERT(inodes[idx].data != NULL, "data buffer allocated");
    ASSERT(memcmp(inodes[idx].data, content, len) == 0,
           "stored content match");
    fs_unmount();
    test_end();
}

static void test_overwrite_file(void) {
    test_start("overwrite file with new content");
    fs_mount();
    ASSERT(fs_create("/overwrite.txt", FS_TYPE_FILE) == 0, "create");

    const char *old = "original data";
    ASSERT(fs_write("/overwrite.txt", old, (uint32_t)(strlen(old) + 1))
           == 0, "write original");

    const char *new = "replacement content that is longer";
    ASSERT(fs_write("/overwrite.txt", new, (uint32_t)(strlen(new) + 1))
           == 0, "write over");

    char buf[128];
    uint32_t out = 0;
    ASSERT(fs_read("/overwrite.txt", buf, sizeof(buf), &out) == 0,
           "read after overwrite");
    ASSERT(memcmp(buf, new, strlen(new) + 1) == 0, "new content");
    fs_unmount();
    test_end();
}

static void test_read_nonexistent_fails(void) {
    test_start("read nonexistent file fails");
    fs_mount();
    char buf[16];
    uint32_t out = 0;
    ASSERT(fs_read("/nope.txt", buf, sizeof(buf), &out) != 0,
           "read should fail");
    fs_unmount();
    test_end();
}

static void test_unlink_file(void) {
    test_start("unlink file");
    fs_mount();
    ASSERT(fs_create("/delete_me.txt", FS_TYPE_FILE) == 0, "create");
    ASSERT(fs_exists("/delete_me.txt") != 0, "exists before unlink");
    ASSERT(fs_unlink("/delete_me.txt") == 0, "unlink");
    ASSERT(fs_exists("/delete_me.txt") == 0, "gone after unlink");
    fs_unmount();
    test_end();
}

static void test_unlink_nonexistent_fails(void) {
    test_start("unlink nonexistent file fails");
    fs_mount();
    ASSERT(fs_unlink("/missing.txt") != 0, "unlink should fail");
    fs_unmount();
    test_end();
}

static void test_rmdir_empty_dir(void) {
    test_start("rmdir empty directory");
    fs_mount();
    ASSERT(fs_create("/emptydir", FS_TYPE_DIR) == 0, "mkdir");
    ASSERT(fs_rmdir("/emptydir") == 0, "rmdir");
    ASSERT(fs_exists("/emptydir") == 0, "gone after rmdir");
    fs_unmount();
    test_end();
}

static void test_rmdir_nonempty_fails(void) {
    test_start("rmdir non-empty directory fails");
    fs_mount();
    ASSERT(fs_create("/mydir", FS_TYPE_DIR) == 0, "mkdir");
    ASSERT(fs_create("/mydir/file.txt", FS_TYPE_FILE) == 0, "create");
    ASSERT(fs_rmdir("/mydir") != 0, "rmdir non-empty should fail");
    ASSERT(fs_exists("/mydir") != 0, "dir still exists");
    /* Delete the file first, now rmdir should work */
    ASSERT(fs_unlink("/mydir/file.txt") == 0, "unlink child");
    ASSERT(fs_rmdir("/mydir") == 0, "rmdir empty should work");
    fs_unmount();
    test_end();
}

static void test_rmdir_root_fails(void) {
    test_start("rmdir root fails");
    fs_mount();
    ASSERT(fs_rmdir("/") != 0, "rmdir / should fail");
    fs_unmount();
    test_end();
}

static void test_truncate_file(void) {
    test_start("truncate file");
    fs_mount();
    ASSERT(fs_create("/t.txt", FS_TYPE_FILE) == 0, "create");
    const char *data = "some data to truncate";
    ASSERT(fs_write("/t.txt", data, (uint32_t)(strlen(data) + 1)) == 0,
           "write");

    ASSERT(fs_truncate("/t.txt", 0) == 0, "truncate to 0");
    int idx = find_inode("/t.txt");
    ASSERT(inodes[idx].size == 0, "size should be 0 after truncate");
    ASSERT(inodes[idx].data == NULL,
           "data buffer freed after truncate");

    fs_unmount();
    test_end();
}

static void test_write_zero_bytes(void) {
    test_start("write zero bytes to file");
    fs_mount();
    ASSERT(fs_create("/zero.txt", FS_TYPE_FILE) == 0, "create");
    ASSERT(fs_write("/zero.txt", "", 0) == 0, "write zero bytes");
    int idx = find_inode("/zero.txt");
    ASSERT(inodes[idx].size == 0, "size is 0");
    fs_unmount();
    test_end();
}

static void test_deeply_nested_dirs(void) {
    test_start("deeply nested directories");
    fs_mount();
    ASSERT(fs_create("/a", FS_TYPE_DIR) == 0, "mkdir /a");
    ASSERT(fs_create("/a/b", FS_TYPE_DIR) == 0, "mkdir /a/b");
    ASSERT(fs_create("/a/b/c", FS_TYPE_DIR) == 0, "mkdir /a/b/c");
    ASSERT(fs_create("/a/b/c/d", FS_TYPE_DIR) == 0, "mkdir /a/b/c/d");
    ASSERT(fs_exists("/a/b/c/d") != 0, "deep path exists");
    int idx = find_inode("/a/b/c/d");
    ASSERT(idx > 0, "deep inode found");
    ASSERT(inodes[idx].type == FS_TYPE_DIR, "deep type dir");
    int parent = (int)inodes[idx].parent;
    ASSERT(parent == find_inode("/a/b/c"), "parent is /a/b/c");
    fs_unmount();
    test_end();
}

static void test_path_with_trailing_slash(void) {
    test_start("path resolution with trailing slash");
    fs_mount();
    ASSERT(fs_create("/trail", FS_TYPE_DIR) == 0, "mkdir");
    ASSERT(fs_create("/trail/", FS_TYPE_DIR) != 0,
           "mkdir with trailing slash (already exists)");
    /* Should still find the dir with trailing slash */
    ASSERT(fs_exists("/trail/") != 0, "find with trailing slash");
    fs_unmount();
    test_end();
}

static void test_multiple_files_in_dir(void) {
    test_start("multiple files in a directory");
    fs_mount();
    ASSERT(fs_create("/files", FS_TYPE_DIR) == 0, "mkdir");
    for (int i = 0; i < 10; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/files/file_%d.txt", i);
        ASSERT(fs_create(path, FS_TYPE_FILE) == 0, "create file");
        ASSERT(fs_exists(path) != 0, "file exists");
    }
    /* Check child count */
    ASSERT(count_children(find_inode("/files")) == 10,
           "exactly 10 children");
    fs_unmount();
    test_end();
}

static void test_create_and_delete_pattern(void) {
    test_start("create and delete alternating pattern");
    fs_mount();
    for (int cycle = 0; cycle < 3; cycle++) {
        char path[64];
        for (int i = 0; i < 20; i++) {
            snprintf(path, sizeof(path), "/f_%d.txt", i);
            if (cycle == 0 || i % 2 == 0) {
                ASSERT(fs_create(path, FS_TYPE_FILE) == 0,
                       "create in cycle");
            }
        }
        /* Delete every other file */
        for (int i = 0; i < 20; i++) {
            snprintf(path, sizeof(path), "/f_%d.txt", i);
            if (fs_exists(path))
                ASSERT(fs_unlink(path) == 0, "unlink");
        }
    }
    /* After all cycles, no files should remain */
    for (int i = 0; i < FS_MAX_INODES; i++) {
        if (inodes[i].in_use && inodes[i].type == FS_TYPE_FILE)
            break; /* found leftover */
    }
    /* Only root should remain */
    int used = 0;
    for (int i = 0; i < FS_MAX_INODES; i++)
        if (inodes[i].in_use) used++;
    ASSERT(used == 1, "only root remains after cleaning up");
    fs_unmount();
    test_end();
}

static void test_max_inodes_exhaustion(void) {
    test_start("inode exhaustion returns error");
    fs_mount();
    /* Fill all inodes except root */
    int created = 0;
    for (int i = 1; i < FS_MAX_INODES; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/f_%d.txt", i);
        if (fs_create(path, FS_TYPE_FILE) == 0)
            created++;
        else
            break;
    }
    ASSERT(created == FS_MAX_INODES - 1, "all inodes consumed");
    /* Next create should fail */
    ASSERT(fs_create("/should_fail.txt", FS_TYPE_FILE) != 0,
           "exhaustion returns error");
    /* Clean up */
    fs_unmount();
    test_end();
}

static void test_write_larger_than_initial_alloc(void) {
    test_start("write larger than initial allocation buffer");
    fs_mount();
    ASSERT(fs_create("/large.txt", FS_TYPE_FILE) == 0, "create");

    /* First write small */
    ASSERT(fs_write("/large.txt", "small", 6) == 0, "small write");

    /* Then write much larger — should reallocate */
    char big[4096];
    memset(big, 'A', sizeof(big));
    ASSERT(fs_write("/large.txt", big, sizeof(big)) == 0,
           "large write");

    char buf[4096];
    uint32_t out = 0;
    ASSERT(fs_read("/large.txt", buf, sizeof(buf), &out) == 0,
           "read after large write");
    ASSERT(out == sizeof(big), "read same size");
    ASSERT(memcmp(buf, big, sizeof(big)) == 0, "large content match");
    fs_unmount();
    test_end();
}

static void test_root_as_parent(void) {
    test_start("all files have root as parent when created in /");
    fs_mount();
    ASSERT(fs_create("/a.txt", FS_TYPE_FILE) == 0, "create a.txt");
    ASSERT(fs_create("/b.txt", FS_TYPE_FILE) == 0, "create b.txt");
    ASSERT(fs_create("/dir1", FS_TYPE_DIR) == 0, "mkdir dir1");
    ASSERT(find_inode("/a.txt") > 0, "a.txt exists");
    ASSERT(find_inode("/b.txt") > 0, "b.txt exists");
    ASSERT(find_inode("/dir1") > 0, "dir1 exists");
    ASSERT(inodes[find_inode("/a.txt")].parent == 0,
           "a.txt parent is root");
    ASSERT(inodes[find_inode("/b.txt")].parent == 0,
           "b.txt parent is root");
    ASSERT(inodes[find_inode("/dir1")].parent == 0,
           "dir1 parent is root");
    fs_unmount();
    test_end();
}

/* Run all tests */
int main(void) {
    printf("============================================\n");
    printf("  Filesystem Operation Unit Tests\n");
    printf("============================================\n\n");

    test_root_exists();
    test_create_file_in_root();
    test_create_directory();
    test_create_in_subdirectory();
    test_create_duplicate_fails();
    test_create_duplicate_dir_fails();
    test_write_and_read_file();
    test_overwrite_file();
    test_read_nonexistent_fails();
    test_unlink_file();
    test_unlink_nonexistent_fails();
    test_rmdir_empty_dir();
    test_rmdir_nonempty_fails();
    test_rmdir_root_fails();
    test_truncate_file();
    test_write_zero_bytes();
    test_deeply_nested_dirs();
    test_path_with_trailing_slash();
    test_multiple_files_in_dir();
    test_create_and_delete_pattern();
    test_max_inodes_exhaustion();
    test_write_larger_than_initial_alloc();
    test_root_as_parent();

    printf("\n============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_total, tests_passed, tests_total - tests_passed);
    printf("============================================\n");

    return (tests_passed == tests_total) ? 0 : 1;
}
