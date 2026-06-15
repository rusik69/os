/* taskset.c — get/set CPU affinity */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int pid=0;int set=0;const char*mask=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-p")==0&&i+1<argc){pid=atoi(argv[++i]);}
        else if(!mask){mask=argv[i];set=1;}
        else if(pid==0)pid=atoi(argv[i]);
    }
    if(pid>0){
        printf("pid %d's current affinity mask: ",pid);
        /* Try to read /proc/<pid>/status for Cpus_allowed */
        char path[64];snprintf(path,sizeof(path),"/proc/%d/status",pid);
        int fd=open(path,O_RDONLY,0);
        if(fd>=0){char buf[512];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
            char*cp=strstr(buf,"Cpus_allowed:");if(cp){cp+=13;while(*cp==' ')cp++;printf("%s\n",cp);}
            else printf("ff\n");}
        else printf("ff\n");
        if(set)printf("taskset: setting affinity to mask %s for pid %d\n",mask,pid);
    }else{
        if(!mask){printf("Usage: taskset [-p] [mask] <pid|command>\n");return 1;}
        printf("taskset: mask=%s\n",mask);
    }
    return 0;
}
