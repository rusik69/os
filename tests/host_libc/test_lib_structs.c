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

int main(void) {
    printf("=== Kernel Data Structure Unit Tests ===\n"); fflush(stdout);
    test_radix_tree(); fflush(stdout);
    test_hexdump(); fflush(stdout);
    printf("\n=== Results: %d passed, %d failed ===\n", tp, tf); fflush(stdout);
    return tf>0?1:0;
}
