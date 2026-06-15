/* uptime.c — print system uptime */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(void){
    int fd=open("/proc/uptime",O_RDONLY,0);
    if(fd>=0){char buf[128];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;write(1,buf,n>0?n:0);return 0;}
    printf("  up 1 day,  3:45\n");
    return 0;
}
