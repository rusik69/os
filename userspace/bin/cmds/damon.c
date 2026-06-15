/* damon.c — Data Access MONitor */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(void){
    printf("DAMON (Data Access Monitor):\n");
    int fd=open("/sys/kernel/debug/damon/nr_regions",O_RDONLY,0);
    if(fd>=0){char buf[64];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;printf("  Regions: %s",buf);}
    else printf("  Not available (use kernel shell)\n");
    return 0;
}
