/* ipcrm.c — Remove IPC resources */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Syscall numbers for IPC */
#define SYS_SHMCTL  230
#define SYS_SEMCTL  231
#define SYS_MSGCTL  232

/* IPC_RMID command */
#define IPC_RMID    0

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage:\n");
        printf("  ipcrm -m <shmid>   (remove shared memory segment)\n");
        printf("  ipcrm -s <semid>   (remove semaphore set)\n");
        printf("  ipcrm -q <msgid>   (remove message queue)\n");
        return 1;
    }

    int id = atoi(argv[2]);
    long ret;

    if (strcmp(argv[1], "-m") == 0) {
        /* Remove shared memory segment */
        __asm__ volatile (
            "syscall"
            : "=a"(ret)
            : "a"((long)SYS_SHMCTL),
              "D"((long)id),
              "S"((long)IPC_RMID),
              "d"(0L)
            : "rcx", "r11", "memory"
        );

        if (ret == 0) {
            printf("ipcrm: removed shared memory segment %d\n", id);
            return 0;
        }

        printf("ipcrm: shmctl(%d, IPC_RMID) returned %ld\n", id, ret);
        return (ret < 0) ? 1 : 0;
    }

    if (strcmp(argv[1], "-s") == 0) {
        /* Remove semaphore set */
        __asm__ volatile (
            "syscall"
            : "=a"(ret)
            : "a"((long)SYS_SEMCTL),
              "D"((long)id),
              "S"((long)IPC_RMID),
              "d"(0L)
            : "rcx", "r11", "memory"
        );

        if (ret == 0) {
            printf("ipcrm: removed semaphore set %d\n", id);
            return 0;
        }

        printf("ipcrm: semctl(%d, IPC_RMID) returned %ld\n", id, ret);
        return (ret < 0) ? 1 : 0;
    }

    if (strcmp(argv[1], "-q") == 0) {
        /* Remove message queue */
        __asm__ volatile (
            "syscall"
            : "=a"(ret)
            : "a"((long)SYS_MSGCTL),
              "D"((long)id),
              "S"((long)IPC_RMID),
              "d"(0L)
            : "rcx", "r11", "memory"
        );

        if (ret == 0) {
            printf("ipcrm: removed message queue %d\n", id);
            return 0;
        }

        printf("ipcrm: msgctl(%d, IPC_RMID) returned %ld\n", id, ret);
        return (ret < 0) ? 1 : 0;
    }

    printf("ipcrm: unknown option '%s' (use -m, -s, or -q)\n", argv[1]);
    return 1;
}
