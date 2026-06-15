/* fbset.c — framebuffer settings */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int xres=0,yres=0,depth=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-x")==0&&i+1<argc)xres=atoi(argv[++i]);
        else if(strcmp(argv[i],"-y")==0&&i+1<argc)yres=atoi(argv[++i]);
        else if(strcmp(argv[i],"-depth")==0&&i+1<argc)depth=atoi(argv[++i]);
    }
    int fd=open("/sys/class/graphics/fb0/virtual_size",O_RDONLY,0);
    if(fd>=0){char buf[128];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;printf("Current: %s",buf);}
    printf("Framebuffer: %dx%dx%d\n",xres?xres:1024,yres?yres:768,depth?depth:32);
    return 0;
}
