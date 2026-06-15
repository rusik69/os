/* pmap.c — display memory map of a process */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: pmap <pid>...\n");return 1;}
    for(int i=1;i<argc;i++){
        char path[64];int p=0;
        const char*pref="/proc/";while(*pref)path[p++]=*pref++;
        const char*pid=argv[i];while(*pid)path[p++]=*pid++;
        const char*suf="/maps";while(*suf)path[p++]=*suf++;
        path[p]=0;
        int fd=open(path,O_RDONLY,0);
        if(fd<0){printf("%s: No such process\n",argv[i]);continue;}
        printf("%s:\n",argv[i]);
        char buf[8192];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;write(1,buf,n);
    }
    return 0;
}
