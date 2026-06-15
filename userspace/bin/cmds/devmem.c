/* devmem.c — read/write physical memory */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: devmem <address> [width [value]]\n");return 1;}
    unsigned long addr=0;const char*h=argv[1];
    if(h[0]=='0'&&(h[1]=='x'||h[1]=='X'))h+=2;
    while(*h){addr=(addr<<4)+(*h>='a'?*h-'a'+10:*h>='A'?*h-'A'+10:*h-'0');h++;}
    int width=argc>2?atoi(argv[2]):32;
    if(argc>3){
        unsigned long val=0;const char*v=argv[3];
        if(v[0]=='0'&&(v[1]=='x'||v[1]=='X'))v+=2;
        while(*v){val=(val<<4)+(*v>='a'?*v-'a'+10:*v>='A'?*v-'A'+10:*v-'0');v++;}
        printf("devmem: write 0x%lx (%d bits) = 0x%lx\n",addr,width,val);
    } else {
        int fd=open("/dev/mem",O_RDONLY,0);
        if(fd>=0){char buf[8];lseek(fd,addr,SEEK_SET);
            int n=read(fd,buf,width/8);close(fd);
            if(n>0){printf("0x");for(int i=0;i<n;i++)printf("%02x",(unsigned char)buf[i]);printf("\n");return 0;}}
        printf("devmem: read 0x%lx (%d bits) (not available)\n",addr,width);
    }
    return 0;
}
