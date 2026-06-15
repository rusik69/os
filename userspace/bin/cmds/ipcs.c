/* ipcs.c — Show IPC resources */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

/* Syscall numbers for IPC */
#define SYS_SHMCTL  230
#define SYS_SEMCTL  231
#define SYS_MSGCTL  232

/* IPC_STAT command */
#define IPC_STAT    1

/* IPC resource IDs to probe (limited range) */

struct shmid_ds {
    unsigned int   shm_perm;  /* dummy */
    unsigned int   shm_segsz;
    unsigned int   shm_cpid;
    unsigned int   shm_lpid;
    unsigned short shm_nattch;
    unsigned short shm_cnattch;
};

struct semid_ds {
    unsigned int   sem_perm;
    unsigned short sem_nsems;
    unsigned short pad;
    unsigned int   sem_otime;
    unsigned int   sem_ctime;
};

struct msqid_ds {
    unsigned int   msg_perm;
    unsigned int   msg_stime;
    unsigned int   msg_rtime;
    unsigned int   msg_ctime;
    unsigned long  msg_cbytes;
    unsigned long  msg_qnum;
    unsigned long  msg_qbytes;
    unsigned short msg_lspid;
    unsigned short msg_lrpid;
};

int main(int argc, char *argv[]) {
    int show_shm = 1;
    int show_sem = 1;
    int show_msg = 1;

    if (argc > 1) {
        show_shm = 0;
        show_sem = 0;
        show_msg = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-m") == 0) show_shm = 1;
            else if (strcmp(argv[i], "-s") == 0) show_sem = 1;
            else if (strcmp(argv[i], "-q") == 0) show_msg = 1;
            else {
                printf("Usage: ipcs [-m] [-s] [-q]\n");
                return 1;
            }
        }
    }

    if (show_shm) {
        printf("------ Shared Memory Segments --------\n");
        printf("key        shmid      owner      perms      bytes      nattch\n");
        /* Try to probe shared memory segments via shmctl syscall */
        int found = 0;
        for (int id = 0; id < 128; id++) {
            struct shmid_ds buf;
            long ret;
            __asm__ volatile (
                "syscall"
                : "=a"(ret)
                : "a"((long)SYS_SHMCTL),
                  "D"((long)id),
                  "S"((long)IPC_STAT),
                  "d"((long)(unsigned long)&buf)
                : "rcx", "r11", "memory"
            );
            if (ret == 0) {
                printf("0x%08x %-10d %-10d %-10d %-10d %-10d\n",
                       0, id, 0, 0, buf.shm_segsz, buf.shm_nattch);
                found = 1;
            }
        }
        if (!found) {
            printf("(none)\n");
        }
    }

    if (show_sem) {
        printf("\n------ Semaphore Arrays --------\n");
        printf("key        semid      owner      perms      nsems\n");
        int found = 0;
        for (int id = 0; id < 128; id++) {
            struct semid_ds buf;
            long ret;
            __asm__ volatile (
                "syscall"
                : "=a"(ret)
                : "a"((long)SYS_SEMCTL),
                  "D"((long)id),
                  "S"((long)IPC_STAT),
                  "d"((long)(unsigned long)&buf)
                : "rcx", "r11", "memory"
            );
            if (ret == 0) {
                printf("0x%08x %-10d %-10d %-10d %-10d\n",
                       0, id, 0, 0, buf.sem_nsems);
                found = 1;
            }
        }
        if (!found) {
            printf("(none)\n");
        }
    }

    if (show_msg) {
        printf("\n------ Message Queues --------\n");
        printf("key        msqid      owner      perms      used-bytes   messages\n");
        int found = 0;
        for (int id = 0; id < 128; id++) {
            struct msqid_ds buf;
            long ret;
            __asm__ volatile (
                "syscall"
                : "=a"(ret)
                : "a"((long)SYS_MSGCTL),
                  "D"((long)id),
                  "S"((long)IPC_STAT),
                  "d"((long)(unsigned long)&buf)
                : "rcx", "r11", "memory"
            );
            if (ret == 0) {
                printf("0x%08x %-10d %-10d %-10d %-10ld %-10ld\n",
                       0, id, 0, 0, buf.msg_cbytes, buf.msg_qnum);
                found = 1;
            }
        }
        if (!found) {
            printf("(none)\n");
        }
    }

    return 0;
}
