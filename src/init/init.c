/* Minimal test init for debugging */
#define SYS_EXIT    4
#define SYS_WRITE   1

static long syscall(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void _start(void) {
    /* Write a single byte to serial via syscall */
    char c = '!';
    syscall(SYS_WRITE, 1, (long)&c, 1);
    syscall(SYS_EXIT, 0, 0, 0);
    for (;;) __asm__ volatile("hlt");
}
