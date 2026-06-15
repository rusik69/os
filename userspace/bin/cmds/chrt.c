/* chrt.c — manipulate real-time attributes of a process */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Syscall numbers */
#define SYS_SCHED_SETSCHEDULER 224
#define SYS_SCHED_GETSCHEDULER 225

/* Scheduling policies */
#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

struct sched_param {
    int sched_priority;
};

static const char *policy_name(int p) {
    switch(p) {
        case SCHED_OTHER: return "SCHED_OTHER";
        case SCHED_FIFO:  return "SCHED_FIFO";
        case SCHED_RR:    return "SCHED_RR";
        default:          return "SCHED_UNKNOWN";
    }
}

static int get_policy(int pid) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_SCHED_GETSCHEDULER),
          "D"((long)pid)
        : "rcx", "r11"
    );
    return (int)ret;
}

static int set_policy(int pid, int policy, int priority) {
    struct sched_param param;
    param.sched_priority = priority;
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_SCHED_SETSCHEDULER),
          "D"((long)pid),
          "S"((long)policy),
          "d"((long)&param)
        : "rcx", "r11", "memory"
    );
    return (int)ret;
}

int main(int argc,char*argv[]){
    int policy=SCHED_OTHER;
    int priority=0;
    int pid=0;
    const char*cmd=0;
    int got_policy=0;

    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-f")==0){policy=SCHED_FIFO;got_policy=1;}
        else if(strcmp(argv[i],"-r")==0){policy=SCHED_RR;got_policy=1;}
        else if(strcmp(argv[i],"-o")==0){policy=SCHED_OTHER;got_policy=1;}
        else if(strcmp(argv[i],"-p")==0&&i+1<argc) pid=atoi(argv[++i]);
        else if(argv[i][0]>='0'&&argv[i][0]<='9'&&!cmd&&!pid) priority=atoi(argv[i]);
        else if(!cmd) cmd=argv[i];
    }

    if(pid>0){
        if(got_policy||priority>0){
            /* Set policy for pid */
            int ret=set_policy(pid,policy,priority);
            if(ret<0){
                printf("chrt: failed to set policy: %s for pid %d\n",policy_name(policy),pid);
                return 1;
            }
            printf("chrt: pid %d's new scheduling policy: %s, priority=%d\n",
                   pid,policy_name(policy),priority);
        }else{
            /* Get current policy */
            int cur=get_policy(pid);
            if(cur>=0){
                printf("chrt: pid %d's current scheduling policy: %s\n",
                       pid,policy_name(cur));
            }else{
                printf("chrt: pid %d's current scheduling policy: SCHED_OTHER (0)\n",pid);
            }
        }
    }else if(cmd){
        /* Fork and set policy for child, then exec */
        int child=fork();
        if(child<0){printf("chrt: fork failed\n");return 1;}
        if(child==0){
            /* Set scheduling for ourself */
            set_policy(0,policy,priority);
            execve(cmd,argv+(int)(cmd-argv[0]),0);
            printf("chrt: cannot exec %s\n",cmd);
            exit(1);
        }
        int status;
        waitpid(child,&status,0);
        return status;
    }else{
        printf("Usage: chrt [-f|-r|-o] <priority> <command> [args]\n");
        printf("       chrt -p <priority> <pid>\n");
        printf("       chrt -p <pid>\n");
    }
    return 0;
}
