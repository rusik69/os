/* lscpu.c — CPU architecture info */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    printf("Architecture:        x86_64\n");
    printf("CPU op-mode(s):     32-bit, 64-bit\n");
    printf("Address sizes:       52 bits physical, 48 bits virtual\n");
    printf("Byte order:         Little Endian\n");
    printf("CPU(s):             1\n");
    int fd=open("/proc/cpuinfo",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
        char*cp=strstr(buf,"model name");if(cp){cp=strchr(cp,':');if(cp){cp+=2;char*eol=strchr(cp,'\n');if(eol)*eol=0;printf("Model name:         %s\n",cp);}}
        cp=strstr(buf,"cache size");if(cp){cp=strchr(cp,':');if(cp){cp+=2;char*eol=strchr(cp,'\n');if(eol)*eol=0;printf("L2 cache:           %s\n",cp);}}
    }
    printf("Vendor ID:          GenuineIntel\n");
    return 0;
}
