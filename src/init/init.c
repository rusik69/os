/*
 * init.c — Minimal userspace init for the OS
 *
 * This is a ring-3 user process that runs as PID 1 after kernel boot.
 * It demonstrates the userspace init infrastructure by launching the
 * kernel-mode shell via a custom syscall, then reaping zombie children.
 *
 * Compile with: x86_64-elf-gcc -ffreestanding -nostdlib -nostdinc \
 *               -Isrc/include -mcmodel=large -O2 -o init.elf init.c
 *
 * Place init.elf on the FAT32 partition at the root.
 */

/* We use only the minimal syscall interface */
#define SYS_EXIT    4
#define SYS_WRITE   1
#define SYS_ELF_EXEC 155
#define SYS_WAITPID 119
#define SYS_YIELD   12

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

/* Simple string operations */
static void print(const char *s) {
    while (*s) s++;
    syscall(SYS_WRITE, 1, (long)s, (long)(s)); /* approximate */
}

/* Write using the kernel's kprintf-compatible syscall.
 * Actually use SYS_WRITE to stdout (fd=1) */
static void putchar(char c) {
    syscall(SYS_WRITE, 1, (long)&c, 1);
}

static void puts(const char *s) {
    while (*s) {
        if (*s == '\n') putchar('\r');
        putchar(*s++);
    }
}

void _start(void) {
    puts("\n*** init: PID 1 running in ring 3 ***\n");

    /* Try to launch the shell as a userspace ELF */
    int ret = (int)syscall(SYS_ELF_EXEC, (long)"/shell.elf", 0, 0);

    if (ret < 0) {
        puts("init: no /shell.elf found, trying /bin/sh.elf\n");
        ret = (int)syscall(SYS_ELF_EXEC, (long)"/bin/sh.elf", 0, 0);
    }

    if (ret < 0) {
        puts("init: no userspace shell found, exiting\n");
    }

    /* Reap children in a loop */
    for (;;) {
        int status = 0;
        long pid = syscall(SYS_WAITPID, -1, (long)&status, 0);
        if (pid > 0) {
            puts("init: reaped child pid ");
            /* Simple number printing */
            char buf[16];
            int i = 15;
            buf[i] = 0;
            long n = pid;
            do {
                buf[--i] = '0' + (n % 10);
                n /= 10;
            } while (n && i > 0);
            puts(&buf[i]);
            puts("\n");
        } else {
            syscall(SYS_YIELD, 0, 0, 0);
        }
    }
}
