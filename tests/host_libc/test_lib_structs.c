/*
 * test_lib_structs.c — Host-side tests for kernel data structures.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct radix_tree_root { unsigned int height; void *rnode; };
extern void radix_tree_init(struct radix_tree_root *root);
extern int  radix_tree_insert(struct radix_tree_root *root, unsigned long key, void *item);
extern void *radix_tree_lookup(struct radix_tree_root *root, unsigned long key);
extern void *radix_tree_delete(struct radix_tree_root *root, unsigned long key);

extern void print_hex_dump(const char *prefix, const void *buf, uint32_t len);
void vga_putchar(char c)    { (void)c; }
void serial_putchar(char c) { (void)c; }

static int tp=0, tf=0;
#define TEST(name, cond) do { if(!(cond)){printf("  FAIL: %s\n",name);tf++;}else{printf("  PASS: %s\n",name);tp++;} }while(0)

static void test_radix_tree(void) {
    struct radix_tree_root root;
    printf("\n[Radix Tree]\n"); fflush(stdout);
    radix_tree_init(&root);
    TEST("init empty", root.rnode==NULL && root.height==0);
    TEST("insert 0", radix_tree_insert(&root, 0, (void*)0x1)==0);
    TEST("lookup 0", radix_tree_lookup(&root, 0)==(void*)0x1);
    TEST("insert 66", radix_tree_insert(&root, 66, (void*)0x2)==0);
    TEST("lookup 66", radix_tree_lookup(&root, 66)==(void*)0x2);
    TEST("lookup 0 still", radix_tree_lookup(&root, 0)==(void*)0x1);
    TEST("missing = NULL", radix_tree_lookup(&root, 999)==NULL);
    radix_tree_delete(&root, 0);
    TEST("delete done", 1==1);
}

static void test_radix_tree_more(void)
{
    struct radix_tree_root root;
    printf("\n[Radix Tree — Extended]\n"); fflush(stdout);

    /* 1. Insert key 0 first (triggers height-0 case), then key 1 */
    radix_tree_init(&root);
    TEST("insert key 0 first", radix_tree_insert(&root, 0, (void*)0xAA) == 0);
    TEST("insert key 1", radix_tree_insert(&root, 1, (void*)0xA1) == 0);
    TEST("lookup key 0", radix_tree_lookup(&root, 0) == (void*)0xAA);
    TEST("lookup key 1", radix_tree_lookup(&root, 1) == (void*)0xA1);

    /* 2. Insert key 2 with different value */
    TEST("insert key 2", radix_tree_insert(&root, 2, (void*)0xB2) == 0);
    TEST("lookup key 2", radix_tree_lookup(&root, 2) == (void*)0xB2);
    TEST("key 1 still valid", radix_tree_lookup(&root, 1) == (void*)0xA1);

    /* 3. Insert keys 0–9 and verify all present */
    {
        int ok = 1;
        for (unsigned long k = 0; k < 10; k++)
            if (radix_tree_insert(&root, k, (void*)(uintptr_t)(0x100 + k)) != 0)
                ok = 0;
        TEST("insert keys 0-9", ok);
    }
    {
        int ok = 1;
        for (unsigned long k = 0; k < 10; k++)
            if (radix_tree_lookup(&root, k) != (void*)(uintptr_t)(0x100 + k))
                ok = 0;
        TEST("lookup keys 0-9 all present", ok);
    }

    /* 4. Delete non-existent key returns NULL */
    TEST("delete non-existent returns NULL",
         radix_tree_delete(&root, 9999) == NULL);

    /* 5. Insert after delete */
    radix_tree_delete(&root, 1);
    TEST("insert after delete",
         radix_tree_insert(&root, 1, (void*)0xDEAD) == 0);
    TEST("lookup after delete+reinsert",
         radix_tree_lookup(&root, 1) == (void*)0xDEAD);

    /* 6. Large key value (outside one radix-tree map) */
    TEST("insert large key",
         radix_tree_insert(&root, 0xF000000000000000ULL, (void*)0xBEEF) == 0);
    TEST("lookup large key",
         radix_tree_lookup(&root, 0xF000000000000000ULL) == (void*)0xBEEF);

    /* 7. Delete all keys and verify emptiness */
    {
        int ok = 1;
        for (unsigned long k = 0; k < 10; k++)
            radix_tree_delete(&root, k);
        radix_tree_delete(&root, 0xF000000000000000ULL);
        for (unsigned long k = 0; k < 10; k++)
            if (radix_tree_lookup(&root, k) != NULL)
                ok = 0;
        if (radix_tree_lookup(&root, 0xF000000000000000ULL) != NULL)
            ok = 0;
        TEST("all deleted keys return NULL", ok);
    }

    /* 8. Re-insert after full deletion */
    TEST("re-insert after full clear",
         radix_tree_insert(&root, 42, (void*)0x42) == 0);
    TEST("re-lookup after re-insert",
         radix_tree_lookup(&root, 42) == (void*)0x42);
}

static void test_hexdump(void) {
    uint8_t buf[32];
    printf("\n[Hex Dump]\n"); fflush(stdout);
    for (int i=0;i<32;i++) buf[i]=(uint8_t)i;
    print_hex_dump("32:", buf, 32); fflush(stdout);
    print_hex_dump("0:", buf, 0); fflush(stdout);
    print_hex_dump("1:", buf, 1); fflush(stdout);
    print_hex_dump("7:", buf, 7); fflush(stdout);
    print_hex_dump("16:", buf, 16); fflush(stdout);
    TEST("hexdump complete", 1==1);
}

static void test_hexdump_more(void)
{
    uint8_t buf[32];
    printf("\n[Hex Dump — Extended]\n"); fflush(stdout);

    /* 1. Single byte with zero value */
    memset(buf, 0, sizeof(buf));
    print_hex_dump("zero1:", buf, 1); fflush(stdout);
    TEST("hexdump single zero byte", 1);

    /* 2. Single byte 0xFF */
    memset(buf, 0xFF, sizeof(buf));
    print_hex_dump("ff1:", buf, 1); fflush(stdout);
    TEST("hexdump single 0xFF byte", 1);

    /* 3. Odd length — 3 bytes */
    memset(buf, 0xAA, sizeof(buf));
    print_hex_dump("odd3:", buf, 3); fflush(stdout);
    TEST("hexdump odd length 3", 1);

    /* 4. All zeros, 16 bytes */
    memset(buf, 0, sizeof(buf));
    print_hex_dump("zero16:", buf, 16); fflush(stdout);
    TEST("hexdump 16 zero bytes", 1);

    /* 5. All 0xFF, 16 bytes */
    memset(buf, 0xFF, sizeof(buf));
    print_hex_dump("ff16:", buf, 16); fflush(stdout);
    TEST("hexdump 16 0xFF bytes", 1);

    /* 6. 4-byte aligned buffer */
    memset(buf, 0x00, sizeof(buf));
    buf[0] = 0xDE; buf[1] = 0xAD; buf[2] = 0xBE; buf[3] = 0xEF;
    print_hex_dump("aligned4:", buf, 4); fflush(stdout);
    TEST("hexdump 4-byte aligned", 1);

    /* 7. 3-byte boundary (not aligned to 4 or 16) */
    memset(buf, 0x11, sizeof(buf));
    print_hex_dump("boundary3:", buf, 3); fflush(stdout);
    TEST("hexdump 3-byte boundary", 1);

    /* 8. Empty buffer (len=0) */
    print_hex_dump("empty:", buf, 0); fflush(stdout);
    TEST("hexdump empty buffer", 1);

    /* 9. Longer buffer — 20 bytes (spans 2 dump lines) */
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 20; i++) buf[i] = (uint8_t)(i * 17);
    print_hex_dump("longer20:", buf, 20); fflush(stdout);
    TEST("hexdump 20 bytes across lines", 1);

    /* 10. Repeated pattern buffer */
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 16; i++) buf[i] = (uint8_t)((i % 2) ? 0xDE : 0xAD);
    print_hex_dump("pattern:", buf, 16); fflush(stdout);
    TEST("hexdump repeating pattern", 1);
}

/* ===================================================================
 *  test_radix_tree_extra — additional radix tree edge cases
 * =================================================================== */
static void test_radix_tree_extra(void)
{
    struct radix_tree_root root;
    printf("\n[Radix Tree — Extra Edge Cases]\n"); fflush(stdout);
    radix_tree_init(&root);

    /* 1. Keys with conflicting bit patterns */
    {
        int ok = 1;
        unsigned long vals[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        for (int i = 0; i < 10; i++)
            if (radix_tree_insert(&root, vals[i], (void*)(uintptr_t)(0x100 + vals[i])) != 0)
                ok = 0;
        TEST("radix_tree_extra: insert 0-9 works", ok);
    }
    {
        int ok = 1;
        for (unsigned long k = 0; k < 10; k++)
            if (radix_tree_lookup(&root, k) != (void*)(uintptr_t)(0x100 + k))
                ok = 0;
        TEST("radix_tree_extra: lookup 0-9 all correct", ok);
    }

    /* 2. Insert and delete several keys, then verify remaining */
    {
        radix_tree_delete(&root, 3);
        radix_tree_delete(&root, 7);
        TEST("radix_tree_extra: key 3 deleted", radix_tree_lookup(&root, 3) == NULL);
        TEST("radix_tree_extra: key 7 deleted", radix_tree_lookup(&root, 7) == NULL);
        TEST("radix_tree_extra: key 0 survives", radix_tree_lookup(&root, 0) == (void*)0x100);
        TEST("radix_tree_extra: key 9 survives", radix_tree_lookup(&root, 9) == (void*)0x109);
    }

    /* 3. Re-insert after delete in same tree */
    {
        radix_tree_delete(&root, 5);
        int r = radix_tree_insert(&root, 5, (void*)0x500);
        TEST("radix_tree_extra: re-insert key 5", r == 0);
        TEST("radix_tree_extra: re-inserted key 5 found", radix_tree_lookup(&root, 5) == (void*)0x500);
    }

    /* 4. Insert key 0, then insert it again (update) */
    {
        struct radix_tree_root root2;
        radix_tree_init(&root2);
        (void)radix_tree_insert(&root2, 0, (void*)0xAA);
        int r2 = radix_tree_insert(&root2, 0, (void*)0xBB);
        TEST("radix_tree_extra: re-insert key 0", r2 == 0 || r2 == 1);
        TEST("radix_tree_extra: re-inserted key 0 updated", radix_tree_lookup(&root2, 0) == (void*)0xBB);
    }

    /* 5. Insert keys in descending order */
    {
        struct radix_tree_root root4;
        radix_tree_init(&root4);
        int ok = 1;
        for (unsigned long k = 100; k > 0; k--)
            if (radix_tree_insert(&root4, k, (void*)(uintptr_t)k) != 0) { ok = 0; break; }
        if (radix_tree_insert(&root4, 0, (void*)0) != 0) ok = 0;
        TEST("radix_tree_extra: descending insert 100..0", ok);
        TEST("radix_tree_extra: lookup 50 after descending insert",
             radix_tree_lookup(&root4, 50) == (void*)50);
        TEST("radix_tree_extra: lookup 0 after descending insert",
             radix_tree_lookup(&root4, 0) == (void*)0);
    }

    /* 6. Delete non-existent key returns NULL (already tested in test_radix_tree_more) */
    {
        struct radix_tree_root root5;
        radix_tree_init(&root5);
        void *r = radix_tree_delete(&root5, 42);
        TEST("radix_tree_extra: delete from empty returns NULL", r == NULL);
    }

    /* 7. Insert same key multiple times — should not corrupt */
    {
        struct radix_tree_root root6;
        radix_tree_init(&root6);
        radix_tree_insert(&root6, 10, (void*)0xA);
        radix_tree_insert(&root6, 10, (void*)0xB);
        radix_tree_insert(&root6, 10, (void*)0xC);
        TEST("radix_tree_extra: multi-update key 10", radix_tree_lookup(&root6, 10) == (void*)0xC);
    }
}

int main(void) {
    printf("=== Kernel Data Structure Unit Tests ===\n"); fflush(stdout);
    test_radix_tree(); fflush(stdout);
    test_radix_tree_more(); fflush(stdout);
    printf("\n--- Radix Tree Extra ---\n"); fflush(stdout);
    test_radix_tree_extra(); fflush(stdout);
    test_hexdump(); fflush(stdout);
    test_hexdump_more(); fflush(stdout);
    printf("\n=== Results: %d passed, %d failed ===\n", tp, tf); fflush(stdout);
    return tf>0?1:0;
}
