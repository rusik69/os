/* schedutils.c — get/set scheduling policy info from /proc */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: schedutils <pid>\n");
        return 1;
    }

    int pid=atoi(argv[1]);
    if(pid<=0) pid=getpid();

    /* Read /proc/<pid>/stat for scheduling info */
    char path[64];
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    int fd=open(path,O_RDONLY,0);
    if(fd<0){
        printf("schedutils: cannot open /proc/%d/stat\n",pid);
        return 1;
    }

    char buf[1024];
    int n=read(fd,buf,sizeof(buf)-1);
    close(fd);
    if(n<=0){printf("schedutils: read error\n");return 1;}
    buf[n]=0;

    /* Parse: pid (comm) state ppid pgrp session tty_nr tpgid flags minflt cminflt
       majflt cmajflt utime stime cutime cstime priority nice num_threads itrealvalue starttime */
    int field=0;
    char *p=buf;
    while(*p&&field<18){
        if(*p==' '){p++;field++;continue;}
        if(field==17){ /* priority */
            int priority=atoi(p);
            /* Skip to nice */
            while(*p&&*p!=' ')p++;
            while(*p==' ')p++;
            int nice_val=atoi(p);
            printf("schedutils: pid=%d priority=%d nice=%d\n",pid,priority,nice_val);
            printf("scheduling: SCHED_OTHER (default)\n");
            return 0;
        }
        p++;
    }

    printf("schedutils: pid=%d default=other\n",pid);
    return 0;
}
