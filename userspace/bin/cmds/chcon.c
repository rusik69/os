/* chcon.c — change security context of a file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

/* Syscall number */
#define SYS_SETXATTR 188

/* Inline syscall for setxattr(path, name, value, size, flags)
   x86-64 ABI: rdi=path, rsi=name, rdx=value, r10=size, r8=flags */
static int setxattr_call(const char *path, const char *name,
                         const void *value, unsigned long size, int flags)
{
    long ret;
    register long r10_val asm("r10") = (long)size;
    register long r8_val asm("r8") = (long)flags;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_SETXATTR),
          "D"((long)path),
          "S"((long)name),
          "d"((long)value),
          "r"(r10_val),
          "r"(r8_val)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: chcon CONTEXT FILE...\n");
        return 1;
    }
    const char *context = argv[1];
    const char *xattr_name = "security.selinux";
    int ret = 0;
    for (int i = 2; i < argc; i++) {
        /* Set SELinux context via setxattr("security.selinux") */
        int err = setxattr_call(argv[i], xattr_name,
                                context, strlen(context), 0);
        if (err == 0) {
            printf("chcon: set context '%s' on '%s'\n", context, argv[i]);
        } else {
            /* Fallback: try writing to /proc/self/attr/<file> */
            char procpath[256];
            snprintf(procpath, sizeof(procpath), "/proc/self/attr/%s", argv[i]);
            int fd = open(procpath, O_WRONLY, 0);
            if (fd >= 0) {
                write(fd, context, strlen(context));
                write(fd, "\n", 1);
                close(fd);
                printf("chcon: set context '%s' on '%s' (via proc attr)\n", context, argv[i]);
            } else {
                printf("chcon: cannot set context on '%s' (no xattr support)\n", argv[i]);
                ret = 1;
            }
        }
    }
    return ret;
}
