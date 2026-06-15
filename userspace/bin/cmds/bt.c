/* bt.c — print kernel backtrace */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    int fd=open("/proc/self/stack",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;write(1,buf,n);return 0;}
    printf("Backtrace via kernel shell 'bt' command\n");
    return 0;
}
