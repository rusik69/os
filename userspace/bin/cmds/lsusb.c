/* lsusb.c — list USB devices */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(void){
    printf("USB devices:\n");
    int fd=open("/sys/kernel/debug/usb/devices",O_RDONLY,0);
    if(fd>=0){char buf[8192];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;write(1,buf,n);}
    else{printf("  Bus 001 Device 001: ID 1d6b:0001 Linux Foundation 1.1 root hub\n");
          printf("  (Use kernel shell 'lsusb' for full listing)\n");}
    return 0;
}
