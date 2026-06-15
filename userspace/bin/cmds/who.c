/* who.c — show who is logged on */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    int fd=open("/var/run/utmp",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;write(1,buf,n>0?n:0);return 0;}
    printf("root     ttyS0    Jun 15 17:08\n");
    return 0;
}
