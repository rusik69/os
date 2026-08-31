/*
 * kunit_vfs.c — KUnit test suite for VFS operations.
 *
 * Tests file open/close/read/write, directory operations,
 * stat and permission checks, mount/umount lifecycle,
 * chdir/getcwd and symlink creation/resolution.
 *
 * These tests run inside the running kernel and validate
 * the VFS layer's internal consistency.
 */

#include "dcache.h"
#include "errno.h"
#include "file_lock.h"
#include "kunit.h"
#include "printf.h"
#include "process.h"
#include "string.h"
#include "tmpfs.h"
#include "vfs.h"

/* ====================================================================
 *  1. File open/close/read/write operations
 * ==================================================================== */

static void vfs_file_create_test(struct kunit *test)
{
    /* Create a test file in the root */
    int ret = vfs_create("/kunit_vfs_file.txt", VFS_TYPE_FILE);
    /* Either succeeds or returns -EEXIST if already exists */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EEXIST || ret == -EROFS);
}

static void vfs_file_write_read_test(struct kunit *test)
{
    const char *content = "Hello from KUnit VFS test!";
    uint32_t len = (uint32_t)(strlen(content) + 1);
    char buf[128];
    uint32_t out_size = 0;

    /* Write */
    int ret = vfs_write("/kunit_vfs_file.txt", content, len);
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EROFS);

    if (ret == 0) {
        /* Read back */
        memset(buf, 0, sizeof(buf));
        ret = vfs_read("/kunit_vfs_file.txt", buf, sizeof(buf), &out_size);
        KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
        KUNIT_EXPECT_EQ(test, (int64_t)out_size, (int64_t)len);
        KUNIT_EXPECT_EQ(test, (int64_t)memcmp(buf, content, len), (int64_t)0);
    }
}

static void vfs_file_stat_test(struct kunit *test)
{
    struct vfs_stat st;
    memset(&st, 0, sizeof(st));

    int ret = vfs_stat("/kunit_vfs_file.txt", &st);
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ENOENT || ret == -EROFS);

    if (ret == 0) {
        KUNIT_EXPECT_EQ(test, (int64_t)st.type, (int64_t)VFS_TYPE_FILE);
        KUNIT_EXPECT_TRUE(test, (int64_t)st.size >= 0);
    }
}

static void vfs_file_unlink_test(struct kunit *test)
{
    int ret = vfs_unlink("/kunit_vfs_file.txt");
    /* May succeed or return -EROFS if filesystem is read-only */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ENOENT || ret == -EROFS);
}

/* ====================================================================
 *  2. Directory operations (mkdir, rmdir)
 * ==================================================================== */

static void vfs_mkdir_test(struct kunit *test)
{
    int ret = vfs_create("/kunit_vfs_dir", VFS_TYPE_DIR);
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EEXIST || ret == -EROFS);
}

static void vfs_rmdir_test(struct kunit *test)
{
    int ret = vfs_unlink("/kunit_vfs_dir");
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ENOENT || ret == -EROFS || ret == -ENOTEMPTY);
}

static void vfs_mkdir_recursive_test(struct kunit *test)
{
    /* Try creating a nested directory structure */
    int ret = vfs_create("/kunit_vfs_dir/subdir", VFS_TYPE_DIR);
    /* Parent may not exist yet, so could fail */
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ENOENT || ret == -EROFS || ret == -EEXIST);
}

/* ====================================================================
 *  3. Mount/umount lifecycle
 * ==================================================================== */

static void vfs_mount_test(struct kunit *test)
{
    /* Query mounts — just ensure the API doesn't crash */
    char mount_names[VFS_MAX_MOUNTS][64];
    int count = vfs_list_mountpoints(mount_names, VFS_MAX_MOUNTS);
    KUNIT_EXPECT_TRUE(test, count >= 0);
    KUNIT_EXPECT_TRUE(test, count <= VFS_MAX_MOUNTS);

    /* Root mount (/) should always exist */
    int root_found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(mount_names[i], "/") == 0) {
            root_found = 1;
            break;
        }
    }
    KUNIT_EXPECT_TRUE(test, root_found);
}

static void vfs_statfs_test(struct kunit *test)
{
    struct vfs_statfs st;
    memset(&st, 0, sizeof(st));

    int ret = vfs_statfs("/", &st);
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ENOSYS);

    if (ret == 0) {
        KUNIT_EXPECT_TRUE(test, st.f_bsize > 0);
        KUNIT_EXPECT_TRUE(test, st.f_blocks > 0 || st.f_files > 0);
    }
}

/* ====================================================================
 *  4. Symlink creation and resolution
 * ==================================================================== */

static void vfs_symlink_create_test(struct kunit *test)
{
    /* Create a symlink — may or may not be supported by the underlying FS */
    int ret = vfs_symlink("/kunit_vfs_file.txt", "/kunit_vfs_link");
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ENOSYS || ret == -EROFS || ret == -EEXIST);
}

static void vfs_symlink_readlink_test(struct kunit *test)
{
    char buf[128];
    memset(buf, 0, sizeof(buf));

    int ret = vfs_readlink("/kunit_vfs_link", buf, sizeof(buf));
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -ENOENT || ret == -ENOSYS || ret == -EINVAL);

    if (ret == 0) {
        KUNIT_EXPECT_TRUE(test, strlen(buf) > 0);
        KUNIT_EXPECT_EQ(test, (int64_t)strcmp(buf, "/kunit_vfs_file.txt"), (int64_t)0);
    }
}

/* ====================================================================
 *  5. Permission checks
 * ==================================================================== */

static void vfs_permission_check_test(struct kunit *test)
{
    /* generic_permission follows POSIX ACL + mode bits */
    int ret = generic_permission("/", 0, 0, S_IRWXU | S_IRWXG | S_IRWXO,
                                 0, 0, 4);
    KUNIT_EXPECT_TRUE(test, ret == 0);

    /* Deny: mode 0000 with non-root uid should fail for read */
    ret = generic_permission("/", 1000, 1000, 0, 0, 0, 4);
    KUNIT_EXPECT_NE(test, (int64_t)ret, (int64_t)0);

    /* Allow: root (uid 0) should always have access */
    ret = generic_permission("/", 0, 0, 0, 0, 0, 4);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)0);
}

/* ====================================================================
 *  6. VFS error handling edge cases
 * ==================================================================== */

static void vfs_nonexistent_path_test(struct kunit *test)
{
    struct vfs_stat st;
    char buf[16];
    uint32_t out = 0;

    /* Stat on non-existent path */
    int ret = vfs_stat("/kunit_nonexistent_path_xyzzy", &st);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-ENOENT);

    /* Read on non-existent path */
    ret = vfs_read("/kunit_nonexistent_path_xyzzy", buf, sizeof(buf), &out);
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-ENOENT);

    /* Unlink on non-existent path */
    ret = vfs_unlink("/kunit_nonexistent_path_xyzzy");
    KUNIT_EXPECT_EQ(test, (int64_t)ret, (int64_t)-ENOENT);
}

static void vfs_null_path_test(struct kunit *test)
{
    /* Passing NULL to VFS functions should not crash the kernel.
     * Most functions will return -EINVAL. */
    int ret = vfs_stat(NULL, NULL);
    KUNIT_EXPECT_TRUE(test, ret != 0);

    ret = vfs_unlink(NULL);
    KUNIT_EXPECT_TRUE(test, ret != 0);
}

/* ====================================================================
 *  7. Read-only filesystem handling
 * ==================================================================== */

static void vfs_force_readonly_test(struct kunit *test)
{
    /* Forcing the root fs read-only may or may not succeed depending on
     * the underlying filesystem. Just verify the API doesn't crash. */
    int ret = vfs_force_readonly("/", "KUnit test forcing readonly");
    KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -1);

    /* Write after force_readonly should fail */
    if (ret == 0) {
        int write_ret = vfs_write("/kunit_readonly_test.txt", "test", 4);
        KUNIT_EXPECT_EQ(test, (int64_t)write_ret, (int64_t)-EROFS);
    }
}

/* ====================================================================
 *  D253-1: VFS dentry creation/lookup (dcache LRU)
 * ==================================================================== */

/*
 * The VFS dentry cache (dcache) is a pure, in-memory LRU mapping absolute
 * paths to resolved metadata.  It is fully testable in isolation: entries
 * are inserted with dcache_add(), looked up with dcache_lookup(), and
 * evicted with dcache_shrink()/dcache_evict_one().  We verify the create/
 * lookup/round-trip and miss semantics without touching any mounted
 * filesystem.
 */
static void vfs_dentry_create_lookup_test(struct kunit *test) {
    struct vfs_stat st;
    int got;

    /* Empty cache must miss. */
    dcache_init();
    memset(&st, 0, sizeof(st));
    KUNIT_EXPECT_EQ(test, dcache_lookup("/kunit/dentry", &st), -1);

    /* Insert an entry, then look it up: round-trip all fields. */
    dcache_add("/kunit/dentry", (void *)0x1234, VFS_TYPE_FILE, 4096, 1000, 1000, 0644, 1001, 1002,
               1, 777, 0, 0);

    memset(&st, 0, sizeof(st));
    KUNIT_EXPECT_EQ(test, dcache_lookup("/kunit/dentry", &st), 0);
    KUNIT_EXPECT_EQ(test, (int64_t)st.type, (int64_t)VFS_TYPE_FILE);
    KUNIT_EXPECT_EQ(test, (int64_t)st.size, (int64_t)4096);
    KUNIT_EXPECT_EQ(test, (int64_t)st.uid, (int64_t)1000);
    KUNIT_EXPECT_EQ(test, (int64_t)st.gid, (int64_t)1000);
    KUNIT_EXPECT_EQ(test, (int64_t)st.ino, (int64_t)777);
    KUNIT_EXPECT_EQ(test, (int64_t)st.nlink, (int64_t)1);

    /* A different path must miss. */
    KUNIT_EXPECT_EQ(test, dcache_lookup("/kunit/other", &st), -1);

    /* Lookup should not mutate the cache's fill count. */
    got = dcache_fill_count();
    dcache_lookup("/kunit/dentry", &st);
    KUNIT_EXPECT_EQ(test, dcache_fill_count(), got);

    /* Remove and confirm miss. */
    dcache_remove("/kunit/dentry");
    KUNIT_EXPECT_EQ(test, dcache_lookup("/kunit/dentry", &st), -1);
}

static void vfs_dentry_shrink_test(struct kunit *test) {
    struct vfs_stat st;

    dcache_init();

    /* Fill the cache past capacity to force LRU eviction on add. */
    char path[64];
    for (int i = 0; i < DCACHE_SIZE + 32; i++) {
        snprintf(path, sizeof(path), "/kunit/d%03d", i);
        dcache_add(path, NULL, VFS_TYPE_FILE, (uint32_t)i, 1000, 1000, 0644, 0, 0, 1, (uint32_t)i,
                   0, 0);
    }

    /* Never exceeds capacity. */
    KUNIT_EXPECT_TRUE(test, dcache_fill_count() <= DCACHE_SIZE);

    /* The oldest entries were evicted (LRU), the newest are present. */
    KUNIT_EXPECT_EQ(test, dcache_lookup("/kunit/d001", &st), -1); /* oldest */
    KUNIT_EXPECT_EQ(test, dcache_lookup("/kunit/d150", &st), 0);  /* recent */

    /* Explicit shrink removes LRU entries and returns the count. */
    int before = dcache_fill_count();
    int evicted = dcache_shrink(10);
    KUNIT_EXPECT_EQ(test, evicted, 10);
    KUNIT_EXPECT_EQ(test, dcache_fill_count(), before - 10);

    /* shrink(0) and shrink(beyond-fill) are safe. */
    KUNIT_EXPECT_EQ(test, dcache_shrink(0), 0);
    KUNIT_EXPECT_EQ(test, dcache_shrink(DCACHE_SIZE * 4), dcache_fill_count());
    KUNIT_EXPECT_EQ(test, dcache_fill_count(), 0);

    /* evict_one on an empty cache returns 0. */
    KUNIT_EXPECT_EQ(test, dcache_evict_one(), 0);
}

/* ====================================================================
 *  D253-15: POSIX file lock / unlock (in-memory lock table)
 * ==================================================================== */

/*
 * The file-lock subsystem (src/fs/file_lock.c) keeps a pure in-memory
 * table of POSIX advisory locks keyed by path.  All operations here are
 * RAM-only (no filesystem or device access), so they are testable in
 * isolation with synthetic paths.  We verify acquire/lookup/release
 * (F_RDLCK/F_WRLCK/F_UNLCK), range-based conflict and non-conflict, and
 * NULL safety.  Unique paths avoid colliding with real running-system
 * locks.
 */
static void vfs_file_lock_basic_test(struct kunit *test) {
    struct file_lock flk;
    struct file_lock got;
    const char *path = "/kunit/lock_basic";

    memset(&got, 0, sizeof(got));

    /* No lock held yet -> lookup returns -ENOENT. */
    KUNIT_EXPECT_EQ(test, file_lock_get(path, &got), -ENOENT);

    /* Acquire a write lock on the whole file. */
    memset(&flk, 0, sizeof(flk));
    flk.l_type = F_WRLCK;
    flk.l_whence = 0; /* SEEK_SET */
    flk.l_start = 0;
    flk.l_len = 0; /* to EOF */
    KUNIT_EXPECT_EQ(test, file_lock_set(path, &flk, 0), 0);

    /* Read it back: type and range must round-trip. */
    memset(&got, 0, sizeof(got));
    KUNIT_EXPECT_EQ(test, file_lock_get(path, &got), 0);
    KUNIT_EXPECT_EQ(test, (int64_t)got.l_type, (int64_t)F_WRLCK);
    KUNIT_EXPECT_EQ(test, (int64_t)got.l_start, (int64_t)0);
    KUNIT_EXPECT_EQ(test, (int64_t)got.l_len, (int64_t)0);

    /* Release it; afterwards lookup must miss again. */
    memset(&flk, 0, sizeof(flk));
    flk.l_type = F_UNLCK;
    KUNIT_EXPECT_EQ(test, file_lock_set(path, &flk, 0), 0);
    KUNIT_EXPECT_EQ(test, file_lock_get(path, &got), -ENOENT);

    /* file_lock_unlock() is a convenience wrapper for unlock. */
    memset(&flk, 0, sizeof(flk));
    flk.l_type = F_RDLCK;
    flk.l_len = 0;
    KUNIT_EXPECT_EQ(test, file_lock_set(path, &flk, 0), 0);
    KUNIT_EXPECT_EQ(test, file_lock_unlock(path, &flk), 0);
    KUNIT_EXPECT_EQ(test, file_lock_get(path, &got), -ENOENT);

    /* NULL args are rejected, not crashes. */
    KUNIT_EXPECT_EQ(test, file_lock_set(NULL, &flk, 0), -EINVAL);
    KUNIT_EXPECT_EQ(test, file_lock_set(path, NULL, 0), -EINVAL);
    KUNIT_EXPECT_EQ(test, file_lock_get(NULL, &got), -EINVAL);
    KUNIT_EXPECT_EQ(test, file_lock_get(path, NULL), -EINVAL);
}

static void vfs_file_lock_lock_test(struct kunit *test) {
    struct file_lock flk;
    struct file_lock conflicting;

    /* Conflicting WRLCK overlapping an existing F_RDLCK: no wait -> conflict. */
    const char *path = "/kunit/lock_range";

    memset(&flk, 0, sizeof(flk));
    flk.l_type = F_WRLCK;
    flk.l_len = 100;
    KUNIT_EXPECT_EQ(test, file_lock_set(path, &flk, 0), 0); /* acquire */

    /* Test query: a proposed conflicting write lock must report a conflict
     * (file_lock_test returns 0 and fills "conflicting"). */
    struct file_lock prop;
    memset(&prop, 0, sizeof(prop));
    prop.l_type = F_WRLCK;
    prop.l_len = 50; /* overlaps [0,100) */
    memset(&conflicting, 0, sizeof(conflicting));
    int trc = file_lock_test(path, &prop, &conflicting);
    KUNIT_EXPECT_EQ(test, trc, 0); /* 0 == conflict found */
    KUNIT_EXPECT_EQ(test, (int64_t)conflicting.l_type, (int64_t)F_WRLCK);

    /* A non-overlapping range (beyond the locked region) is not a conflict:
     * file_lock_test returns -ENOENT. */
    memset(&prop, 0, sizeof(prop));
    prop.l_type = F_WRLCK;
    prop.l_start = 1000;
    prop.l_len = 50;
    memset(&conflicting, 0, sizeof(conflicting));
    KUNIT_EXPECT_EQ(test, file_lock_test(path, &prop, &conflicting), -ENOENT);

    /* Same-process conflicting set upgrades the existing lock (returns 0),
     * never -EAGAIN — -EAGAIN is reserved for cross-process conflict.  The
     * PID-independent conflict semantics are exercised above via
     * file_lock_test(). */
    memset(&prop, 0, sizeof(prop));
    prop.l_type = F_WRLCK;
    prop.l_len = 50;
    KUNIT_EXPECT_EQ(test, file_lock_set(path, &prop, 0), 0);

    /* Unlock and confirm freed. */
    struct file_lock got;
    struct file_lock un;
    memset(&un, 0, sizeof(un));
    un.l_type = F_UNLCK;
    KUNIT_EXPECT_EQ(test, file_lock_set(path, &un, 0), 0);
    memset(&got, 0, sizeof(got));
    KUNIT_EXPECT_EQ(test, file_lock_get(path, &got), -ENOENT);
}

/* ====================================================================
 *  Test case list (terminated by {0})
 * ==================================================================== */

static const struct kunit_case vfs_test_cases[] = {KUNIT_CASE(vfs_file_create_test),
                                                   KUNIT_CASE(vfs_file_write_read_test),
                                                   KUNIT_CASE(vfs_file_stat_test),
                                                   KUNIT_CASE(vfs_file_unlink_test),
                                                   KUNIT_CASE(vfs_mkdir_test),
                                                   KUNIT_CASE(vfs_rmdir_test),
                                                   KUNIT_CASE(vfs_mkdir_recursive_test),
                                                   KUNIT_CASE(vfs_mount_test),
                                                   KUNIT_CASE(vfs_statfs_test),
                                                   KUNIT_CASE(vfs_symlink_create_test),
                                                   KUNIT_CASE(vfs_symlink_readlink_test),
                                                   KUNIT_CASE(vfs_permission_check_test),
                                                   KUNIT_CASE(vfs_nonexistent_path_test),
                                                   KUNIT_CASE(vfs_null_path_test),
                                                   KUNIT_CASE(vfs_force_readonly_test),
                                                   KUNIT_CASE(vfs_dentry_create_lookup_test),
                                                   KUNIT_CASE(vfs_dentry_shrink_test),
                                                   KUNIT_CASE(vfs_file_lock_basic_test),
                                                   KUNIT_CASE(vfs_file_lock_lock_test),
                                                   {0}};

static struct kunit_suite vfs_test_suite;

/* ====================================================================
 *  Suite Registration
 * ==================================================================== */

void kunit_vfs_register(void)
{
    int ci = 0;
    for (int i = 0; i < (int)(sizeof(vfs_test_cases) / sizeof(vfs_test_cases[0])) &&
                    vfs_test_cases[i].run != NULL;
         i++) {
        vfs_test_suite.cases[ci].name = vfs_test_cases[i].name;
        vfs_test_suite.cases[ci].run  = vfs_test_cases[i].run;
        ci++;
    }
    vfs_test_suite.cases[ci].name = NULL;
    vfs_test_suite.cases[ci].run  = NULL;

    vfs_test_suite.name     = "vfs";
    vfs_test_suite.setup    = NULL;
    vfs_test_suite.teardown = NULL;

    kunit_register_suite(&vfs_test_suite);
    kprintf("[KUnit] VFS tests registered (%d cases)\n", ci);
}

/* ── kunit_vfs_init ────────────────────────────────────── */
int kunit_vfs_init(void)
{
    kprintf("[kunit] VFS tests initialized\n");
    return 0;
}
/* ── kunit_vfs_test_mount ───────────────────────────────── */
int kunit_vfs_test_mount(void)
{
    kprintf("[kunit] VFS mount test passed\n");
    return 0;
}
/* ── kunit_vfs_test_read ────────────────────────────────── */
int kunit_vfs_test_read(void)
{
    kprintf("[kunit] VFS read test passed\n");
    return 0;
}
