/* iostat.c — I/O statistics */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(void){
    printf("Device:            tps    MB_read/s    MB_wrtn/s\n");
    int fd=open("/proc/diskstats",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
        write(1,buf,n);}
    return 0;
}
