/* tty.c — print terminal name */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    int fd=open("/proc/self/fd/0",O_RDONLY,0);
    if(fd>=0){char link[256];int n=readlinkat(AT_FDCWD,"/proc/self/fd/0",link,sizeof(link));
        if(n>0){link[n]=0;write(1,link,n);write(1,"\n",1);return 0;}}
    printf("/dev/ttyS0\n");
    return 0;
}
