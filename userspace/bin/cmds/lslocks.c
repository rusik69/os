/* lslocks.c — list local file locks */
#include "unistd.h"
#include "stdio.h"

int main(void){
    printf("ACTIVE LOCKS:\n");
    int fd=open("/proc/locks",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
        if(n>0)write(1,buf,n);
        else printf("  No locks (empty /proc/locks)\n");
    }else printf("  No locks (/proc/locks not available)\n");
    return 0;
}
