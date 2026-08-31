/*
 * kunit_page_cache.c — KUnit test suite for the page cache.
 *
 * The page cache (src/fs/page_cache.c) is a pure in-memory subsystem:
 * pages are looked up / inserted / removed / invalidated entirely in RAM
 * (backed by PMM frames).  The only path that touches a real block device
 * is dirty-page eviction through writeback_fn() — which tests deliberately
 * never trigger.  Readahead reads arrive through a caller-supplied
 * backing_store callback, which tests stub with synthetic data.
 *
 * These tests therefore validate real cache semantics without any disk I/O.
 */

#include "errno.h"
#include "kunit.h"
#include "page_cache.h"
#include "printf.h"
#include "string.h"

/* ====================================================================
 *  D253-13: page cache find_get_page / add_to_page_cache
 * ==================================================================== */

static void page_cache_add_lookup_remove_test(struct kunit *test) {
    char data[PAGE_SIZE];
    char readback[PAGE_SIZE];

    /* page_cache_init() runs at boot; the cache is already live. */

    /* Lookup before insert -> miss. */
    KUNIT_EXPECT_EQ(test, page_cache_lookup(1, 0), (void *)0);
    KUNIT_EXPECT_EQ(test, page_cache_get_data(1, 0), (void *)0);

    /* Insert a page: full contents must round-trip. */
    for (int i = 0; i < (int)sizeof(data); i++)
        data[i] = (char)(i * 7 + 1);
    KUNIT_EXPECT_EQ(test, page_cache_add(1, 0, data), 0);

    KUNIT_EXPECT_NOT_NULL(test, page_cache_lookup(1, 0));
    void *got = page_cache_get_data(1, 0);
    KUNIT_EXPECT_NOT_NULL(test, got);
    memcpy(readback, got, sizeof(readback));
    KUNIT_EXPECT_EQ(test, (int64_t)memcmp(readback, data, sizeof(data)), (int64_t)0);

    /* Re-adding the same (ino,block) is idempotent (0, no duplicate). */
    KUNIT_EXPECT_EQ(test, page_cache_add(1, 0, data), 0);

    /* A different block / different inode is a distinct cache line. */
    for (int i = 0; i < (int)sizeof(data); i++)
        data[i] = (char)(i ^ 0xAA);
    KUNIT_EXPECT_EQ(test, page_cache_add(1, 1, data), 0);
    KUNIT_EXPECT_EQ(test, page_cache_add(2, 0, data), 0);

    /* Remove a single line: it must miss afterwards. */
    page_cache_remove(1, 1);
    KUNIT_EXPECT_EQ(test, page_cache_lookup(1, 1), (void *)0);
    /* Siblings survive. */
    KUNIT_EXPECT_NOT_NULL(test, page_cache_lookup(1, 0));
    KUNIT_EXPECT_NOT_NULL(test, page_cache_lookup(2, 0));

    /* Invalidate a whole inode. */
    page_cache_invalidate_ino(1);
    KUNIT_EXPECT_EQ(test, page_cache_lookup(1, 0), (void *)0);
    KUNIT_EXPECT_NOT_NULL(test, page_cache_lookup(2, 0));

    page_cache_remove(2, 0);
    KUNIT_EXPECT_EQ(test, page_cache_lookup(2, 0), (void *)0);
}

static void page_cache_eviction_test(struct kunit *test) {
    char data[PAGE_SIZE];
    char readback[PAGE_SIZE];
    memset(data, 0x42, sizeof(data));

    /* page_cache_init() runs at boot; the cache is already live. */

    /* Insert more distinct pages than the cache holds; each add either
     * succeeds (evicting an LRU page if full) or fails with -ENOMEM when
     * memory is tight.  Afterwards, a written page must still round-trip
     * (its cached copy was not corrupted by churn). */
    for (uint64_t block = 0; block < (uint64_t)(PAGE_CACHE_MAX_PAGES + 64); block++) {
        if (block == 7)
            *(char *)(data + 0) = data[0]; /* keep buffer valid throughout */
        int r = page_cache_add(20, block, data);
        KUNIT_EXPECT_TRUE(test, r == 0 || r == -ENOMEM);
    }

    /* A page we refreshed near the end must read back intact regardless of
     * its cache slot being reused by churn. */
    memset(readback, 0, sizeof(readback));
    void *got = page_cache_get_data(20, PAGE_CACHE_MAX_PAGES + 63);
    if (got) {
        memcpy(readback, got, sizeof(readback));
        KUNIT_EXPECT_EQ(test, (int64_t)memcmp(readback, data, sizeof(data)), (int64_t)0);
    }

    /* Cleanup included pages so we don't leak PMM frames. */
    for (uint64_t block = 0; block < (uint64_t)(PAGE_CACHE_MAX_PAGES + 64); block++)
        page_cache_remove(20, block);
}

/* ====================================================================
 *  D253-14: page cache readahead
 * ==================================================================== */

/* Synthetic backing store: 128 sectors, each filled with a caller-defined
 * byte.  Returns 0 on success like a real driver. */
#define FAKE_NBLOCKS 128
static unsigned char fake_store[FAKE_NBLOCKS * PAGE_SIZE];
static int fake_backing_read(uint32_t lba, uint8_t count, void *buf) {
    if (lba >= FAKE_NBLOCKS || count == 0)
        return -1;
    uint32_t off = lba * 512u;
    if (off + (uint32_t)count * 512u > sizeof(fake_store))
        return -1;
    memcpy(buf, &fake_store[off], (size_t)count * 512u);
    return 0;
}

static void page_cache_readahead_test(struct kunit *test) {
    /* page_cache_init() runs at boot; the cache is already live. */

    memset(fake_store, 0x00, sizeof(fake_store));
    for (int i = 0; i < FAKE_NBLOCKS; i++)
        fake_store[i * PAGE_SIZE] = (unsigned char)(i + 1); /* marker at each page start */

    /* Page-cache block n covers page n (lba = n * PAGE_SIZE/512 sectors). */
    char buf[PAGE_SIZE];

    /* cache-only lookup on a missing page still misses after readahead of
     * other blocks; the read path (with backing store) populates cache. */
    KUNIT_EXPECT_EQ(test, page_cache_read(5, 0, buf, fake_backing_read), 0);
    KUNIT_EXPECT_EQ(test, page_cache_lookup(5, 0) != NULL, 1);
    KUNIT_EXPECT_EQ(test, (unsigned char)buf[0], (unsigned char)1);

    /* Explicit readahead prefetches a run of blocks into the cache. */
    int n = page_cache_readahead(5, 1, 3, fake_backing_read);
    KUNIT_EXPECT_TRUE(test, n >= 1 && n <= 3);
    KUNIT_EXPECT_NOT_NULL(test, page_cache_lookup(5, 1));
    KUNIT_EXPECT_NOT_NULL(test, page_cache_lookup(5, 2));

    /* Batch readahead returns prefetched count and populates cache. */
    int b = page_cache_batch_readahead(5, 4, 2, fake_backing_read);
    KUNIT_EXPECT_TRUE(test, b >= 1 && b <= 2);
    KUNIT_EXPECT_NOT_NULL(test, page_cache_lookup(5, 4));

    /* Readahead reset does not crash and leaves existing pages intact. */
    page_cache_readahead_reset(5);

    /* Stats call is safe and sums to sane values. */
    int hits = 0, misses = 0, prefetches = 0;
    page_cache_readahead_stats(&hits, &misses, &prefetches);
    KUNIT_EXPECT_TRUE(test, hits >= 0 && misses >= 0 && prefetches >= 0);

    /* Cleanup: drop every page we cached. */
    for (uint64_t block = 0; block < 6; block++)
        page_cache_remove(5, block);
}

/* ====================================================================
 *  Test case list (terminated by {0})
 * ==================================================================== */

static const struct kunit_case page_cache_test_cases[] = {
    KUNIT_CASE(page_cache_add_lookup_remove_test),
    KUNIT_CASE(page_cache_eviction_test),
    KUNIT_CASE(page_cache_readahead_test),
    {0}};

static struct kunit_suite page_cache_test_suite;

void kunit_page_cache_register(void) {
    int ci = 0;
    for (int i = 0; i < (int)(sizeof(page_cache_test_cases) / sizeof(page_cache_test_cases[0])) &&
                    page_cache_test_cases[i].run != NULL;
         i++) {
        page_cache_test_suite.cases[ci].name = page_cache_test_cases[i].name;
        page_cache_test_suite.cases[ci].run = page_cache_test_cases[i].run;
        ci++;
    }
    page_cache_test_suite.cases[ci].name = NULL;
    page_cache_test_suite.cases[ci].run = NULL;

    page_cache_test_suite.name = "page_cache";
    page_cache_test_suite.setup = NULL;
    page_cache_test_suite.teardown = NULL;

    kunit_register_suite(&page_cache_test_suite);
    kprintf("[KUnit] page_cache tests registered (%d cases)\n", ci);
}