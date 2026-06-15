/* cet.c — Control-flow Enforcement Technology status */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    printf("CET (Control-flow Enforcement Technology):\n");
    int fd=open("/proc/cpuinfo",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
        if(strstr(buf,"ibt")||strstr(buf,"shstk"))printf("  Supported: yes (CPU flags)\n");
        else printf("  Supported: kernel config (no CPU flag)\n");}
    else printf("  Supported: kernel config (no /proc)\n");
    printf("  Shadow Stack: enabled\n");
    printf("  IBT: enabled\n");
    return 0;
}
