/* fg.c — bring job to foreground (show process info) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int job_id = -1;
    if(argc>1&&argv[1][0]=='%') job_id=atoi(argv[1]+1);
    else if(argc>1) job_id=atoi(argv[1]);

    /* Read /proc to find processes */
    int fd = open("/proc",O_RDONLY,0);
    if(fd<0){
        printf("fg: no /proc available\n");
        return 1;
    }

    char buf[4096];
    int n = getdents64(fd,buf,sizeof(buf));
    close(fd);

    if(n<=0){
        printf("fg: no processes\n");
        return 0;
    }

    int found=0;
    unsigned long off=0;
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
                    if(job_id<=0||job_id==pid){
                        printf("[%d] %s\n",pid,cmd);
                        found=1;
                        if(job_id>0) break;
                    }
                }
            }
        }
        off+=de->d_reclen;
    }

    if(!found){
        if(job_id>0) printf("fg: job %d not found\n",job_id);
        else printf("fg: no foreground jobs\n");
    }
    return 0;
}
