/* hostname.c — print/set hostname */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc,char*argv[]){
    if(argc>1){
        /* Set hostname by writing to /proc/sys/kernel/hostname */
        int fd=open("/proc/sys/kernel/hostname",O_WRONLY,0);
        if(fd>=0){
            write(fd,argv[1],strlen(argv[1]));
            close(fd);
            printf("hostname: set to '%s'\n",argv[1]);
            return 0;
        }
        printf("hostname: cannot set hostname (no /proc/sys)\n");
        return 1;
    }

    /* Get hostname */
    char name[65];
    if(gethostname(name,sizeof(name))>=0){
        write(1,name,strlen(name));
        write(1,"\n",1);
        return 0;
    }
    printf("hostname\n");
    return 1;
}
