/* lsmod.c — list loaded kernel modules */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(void){
    int fd=open("/proc/modules",O_RDONLY,0);
    if(fd<0){printf("lsmod: /proc/modules not available\n");return 1;}
    char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
    write(1,buf,n);
    return 0;
}
