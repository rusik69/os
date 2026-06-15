/* lsusb.c — list USB devices */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    printf("USB Devices:\n");
    int fd=open("/sys/bus/usb/devices",O_RDONLY,0);
    if(fd>=0){
        char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
        char*cp=buf;
        while(cp&&*cp){
            char name[64];int i=0;
            while(*cp&&*cp!='\n'&&i<63)name[i++]=*cp++;
            name[i]=0;if(*cp=='\n')cp++;
            if(name[0]!='.')printf("  %s\n",name);
        }
    }else printf("  No USB subsystem (kernel shell 'lsusb' for details)\n");
    return 0;
}
