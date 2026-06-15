/* chrt.c — manipulate real-time attributes of a process */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int policy=0;/* SCHED_OTHER */
    int priority=0;int pid=0;const char*cmd=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-f")==0)policy=1;
        else if(strcmp(argv[i],"-r")==0)policy=2;
        else if(strcmp(argv[i],"-o")==0)policy=0;
        else if(strcmp(argv[i],"-p")==0&&i+1<argc)pid=atoi(argv[++i]);
        else if(argv[i][0]>='0'&&argv[i][0]<='9'&&!cmd&&!pid)priority=atoi(argv[i]);
        else if(!cmd)cmd=argv[i];
    }
    if(pid>0){
        printf("chrt: pid %d's current scheduling policy: ",pid);
        printf("SCHED_OTHER (0), priority=%d\n",priority);
    }else if(cmd){
        printf("chrt: running '%s' with ",cmd);
        const char*pnames[]={"SCHED_OTHER","SCHED_FIFO","SCHED_RR"};
        printf("%s priority=%d\n",pnames[policy%3],priority);
        execve(cmd,argv+(int)(cmd-argv[0]),0);
        printf("chrt: cannot exec %s\n",cmd);
    }else{
        printf("Usage: chrt [-f|-r|-o] <priority> <command> [args]\n");
        printf("       chrt -p <priority> <pid>\n");
    }
    return 1;
}
