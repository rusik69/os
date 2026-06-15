/* link.c — create a hard link */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* The kernel may not have SYS_LINK; we use a raw syscall.
 * Linux x86_64 SYS_LINK = 86. If kernel doesn't support it, returns -1. */
static int link_syscall(const char *target, const char *linkname) {
    long ret;
    /* syscall 86: link(target, linkname) */
    asm volatile("movl $86, %%eax; syscall" : "=a"(ret) : "D"(target), "S"(linkname) : "rcx", "r11", "memory");
    return (int)ret;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("usage: link <target> <linkname>\n");
        return 1;
    }

    const char *target = argv[1];
    const char *linkname = argv[2];

    int ret = link_syscall(target, linkname);
    if (ret < 0) {
        printf("link: cannot create link '%s' -> '%s' (syscall returned %d)\n",
               linkname, target, ret);
        printf("link: hard link syscall not supported in this kernel\n");
        return 1;
    }

    printf("link: created '%s' -> '%s'\n", linkname, target);
    return 0;
}
