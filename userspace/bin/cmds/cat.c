/* cat.c — concatenate files and print to stdout */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2){
        char buf[4096];long n;
        while((n=read(0,buf,sizeof(buf)))>0)write(1,buf,n);
        return 0;
    }
    for(int i=1;i<argc;i++){
        int fd=open(argv[i],O_RDONLY,0);
        if(fd<0){printf("cat: %s: No such file\n",argv[i]);return 1;}
        char buf[4096];long n;
        while((n=read(fd,buf,sizeof(buf)))>0)write(1,buf,n);
        close(fd);
    }
    return 0;
}
