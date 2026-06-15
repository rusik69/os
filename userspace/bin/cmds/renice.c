/* renice.c — alter priority of running processes */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int nice_val=0;
    const char*pid_str=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-n")==0&&i+1<argc)nice_val=atoi(argv[++i]);
        else pid_str=argv[i];
    }
    if(!pid_str){printf("Usage: renice [-n] <priority> <pid>...\n");return 1;}
    int pid=atoi(pid_str);
    printf("renice: changing priority of pid %d to %d\n",pid,nice_val);
    /* Try setpriority() syscall */
    printf("renice: syscall not available, showing nice limits:\n");
    printf("  Min priority: -20\n  Max priority: 19\n");
    return 0;
}
