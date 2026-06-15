/* nvme.c — NVMe storage control */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    printf("NVMe devices:\n");
    int fd=open("/sys/class/nvme",O_RDONLY,0);
    if(fd>=0){
        char buf[4096];read(fd,buf,sizeof(buf)-1);close(fd);
        write(1,buf,strlen(buf));
    }else{
        printf("  /sys/class/nvme not available (use kernel shell 'nvme')\n");
    }
    return 0;
}
