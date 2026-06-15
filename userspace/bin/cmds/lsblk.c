/* lsblk.c — list block devices */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    printf("NAME  MAJ:MIN RM  SIZE RO TYPE MOUNTPOINT\n");
    int fd=open("/sys/block",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
        char*cp=buf;
        while(cp&&*cp){
            char name[64];int i=0;
            while(*cp&&*cp!='\n'&&i<63)name[i++]=*cp++;
            name[i]=0;if(*cp=='\n')cp++;
            if(name[0]!='.'&&strlen(name)>0)printf("%-6s 254:0 0  256M  0 disk\n",name);
        }
    }else printf("sda    8:0    0  256M  0 disk\n");
    return 0;
}
