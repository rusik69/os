/* jobs.c — list active processes from /proc */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    (void)argc;(void)argv;

    int fd = open("/proc",O_RDONLY,0);
    if(fd<0){
        printf("jobs: no /proc available\n");
        return 1;
    }

    char buf[4096];
    int n = getdents64(fd,buf,sizeof(buf));
    close(fd);

    if(n<=0){
        printf("No active background jobs\n");
        return 0;
    }

    printf("PID\tSTATUS\tCMD\n");
    unsigned long off=0;
    int count=0;
    while(off<(unsigned long)n){
        struct dirent *de = (struct dirent*)(buf+off);
        if(de->d_name[0]>='0'&&de->d_name[0]<='9'){
            int pid=atoi(de->d_name);
            if(pid==getpid()||pid==getppid()){off+=de->d_reclen;continue;}
            /* Read cmdline */
            char cmdpath[64];
            snprintf(cmdpath,sizeof(cmdpath),"/proc/%s/cmdline",de->d_name);
            int cfd=open(cmdpath,O_RDONLY,0);
            if(cfd>=0){
                char cmd[256];
                int cn=read(cfd,cmd,sizeof(cmd)-1);
                close(cfd);
                if(cn>0){
                    cmd[cn]=0;
                    for(int i=0;i<cn-1;i++) if(cmd[i]==0) cmd[i]=' ';
                    printf("[%d]\tRunning\t%s\n",pid,cmd);
                    count++;
                }
            }
        }
        off+=de->d_reclen;
    }

    if(count==0) printf("No active background jobs\n");
    return 0;
}
