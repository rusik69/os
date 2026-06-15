/* jobs.c — list active jobs */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    printf("Active jobs:\n");
    int fd=open("/proc/self/status",O_RDONLY,0);
    if(fd>=0){char buf[512];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
        char*cp=strstr(buf,"Pid:");if(cp)printf("  [%d] Running\n",atoi(cp+4));}
    else printf("  No active background jobs\n");
    return 0;
}
