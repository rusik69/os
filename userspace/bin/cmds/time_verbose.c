/* time_verbose.c — time with more detail */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: time_verbose <command> [args...]\n");
        return 1;
    }
    struct timespec t1, t2;
    clock_gettime(0, &t1);
    int pid = fork();
    if (pid < 0) {
        printf("time_verbose: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        execve(argv[1], argv + 1, 0);
        printf("time_verbose: cannot exec %s\n", argv[1]);
        exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    clock_gettime(0, &t2);
    unsigned long long sec = t2.tv_sec - t1.tv_sec;
    unsigned long long nsec;
    if (t2.tv_nsec >= t1.tv_nsec)
        nsec = t2.tv_nsec - t1.tv_nsec;
    else {
        sec--;
        nsec = 1000000000ULL + t2.tv_nsec - t1.tv_nsec;
    }
    printf("\nreal\t%llu.%09llu\n", sec, nsec);
    int sig = 0;
    if (status & 0x7f) sig = status & 0x7f;
    int code = (status >> 8) & 0xff;
    if (sig) printf("signal\t%d\n", sig);
    else printf("exit\t%d\n", code);
    return 0;
}
