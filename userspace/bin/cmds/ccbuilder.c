/* ccbuilder.c — build manifest runner */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: ccbuilder <manifest>\n");return 1;}
    printf("ccbuilder: reading manifest %s\n",argv[1]);
    int fd=open(argv[1],O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;write(1,buf,n);}
    else printf("ccbuilder: cannot open %s\n",argv[1]);
    return 0;
}
