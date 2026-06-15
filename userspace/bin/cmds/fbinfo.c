/* fbinfo.c — framebuffer information */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(void){
    printf("Framebuffer information:\n");
    int fd=open("/sys/class/graphics/fb0/name",O_RDONLY,0);
    if(fd>=0){char buf[128];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;printf("  Name: %s",buf);}
    fd=open("/sys/class/graphics/fb0/virtual_size",O_RDONLY,0);
    if(fd>=0){char buf[128];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;printf("  Size: %s",buf);}
    printf("  Depth: 32 bpp\n");
    return 0;
}
