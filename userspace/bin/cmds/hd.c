/* hd.c — hex dump */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    int fd=0;if(argc>1){fd=open(argv[1],O_RDONLY,0);if(fd<0){printf("hd: %s: No such file\n",argv[1]);return 1;}}
    unsigned char buf[16];long n;unsigned long addr=0;
    while((n=read(fd,buf,sizeof(buf)))>0){
        printf("%08lx  ",addr);
        int i;for(i=0;i<16;i++){if(i<n)printf("%02x ",buf[i]);else printf("   ");if(i==7)printf(" ");}
        printf(" |");for(i=0;i<n;i++){char c=buf[i];putchar(c>=32&&c<127?c:'.');}
        printf("|\n");addr+=16;
    }
    if(argc>1)close(fd);
    return 0;
}
