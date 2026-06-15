/* unshare.c — unshare namespaces and exec a command */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

/* Syscall number for unshare */
#define SYS_UNSHARE 394

/* Namespace clone flags */
#define CLONE_NEWNS      0x00020000
#define CLONE_NEWUTS     0x04000000
#define CLONE_NEWPID     0x20000000
#define CLONE_NEWNET     0x40000000
#define CLONE_NEWIPC     0x08000000

/* Raw syscall wrapper for unshare(flags) */
static long unshare_call(unsigned long flags) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_UNSHARE),
          "D"((long)flags)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static void usage(void) {
    printf("usage: unshare [--mount] [--uts] [--ipc] [--net] [--pid] <command [args...]>\n");
    printf("Disassociate namespaces and run a command in isolation.\n");
    printf("  --mount    unshare mount namespace\n");
    printf("  --uts      unshare hostname/domain namespace\n");
    printf("  --ipc      unshare IPC namespace\n");
    printf("  --net      unshare network namespace\n");
    printf("  --pid      unshare PID namespace\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage();
        return 1;
    }

    unsigned long ns_flags = 0;
    int cmd_idx = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mount") == 0)
            ns_flags |= CLONE_NEWNS;
        else if (strcmp(argv[i], "--uts") == 0)
            ns_flags |= CLONE_NEWUTS;
        else if (strcmp(argv[i], "--ipc") == 0)
            ns_flags |= CLONE_NEWIPC;
        else if (strcmp(argv[i], "--net") == 0)
            ns_flags |= CLONE_NEWNET;
        else if (strcmp(argv[i], "--pid") == 0)
            ns_flags |= CLONE_NEWPID;
        else {
            cmd_idx = i;
            break;
        }
    }

    if (cmd_idx < 0) {
        printf("unshare: no command specified\n");
        usage();
        return 1;
    }

    /* Call the unshare syscall */
    long ret = unshare_call(ns_flags);
    if (ret < 0) {
        printf("unshare: failed (errno=%ld)\n", -ret);
        return 1;
    }

    /* Fork and exec the command */
    int pid = fork();
    if (pid < 0) {
        printf("unshare: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: exec the command */
        execve(argv[cmd_idx], argv + cmd_idx, NULL);
        printf("unshare: exec '%s' failed\n", argv[cmd_idx]);
        return 1;
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    if (status != 0)
        return status;
    return 0;
}
